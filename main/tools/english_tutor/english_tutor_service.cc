#include "english_tutor_service.h"

#include <cJSON.h>
#include <esp_log.h>

#include <algorithm>
#include <memory>

#include <http.h>
#include <network_interface.h>

#include "board.h"
#include "cjson_utils.h"

#include "../service_config.h"

#define TAG "EnglishTutorService"

namespace {

constexpr int kHttpTimeoutMs = 15000;
constexpr size_t kMaxBodyBytes = 4 * 1024;
constexpr size_t kReadChunkBytes = 512;
constexpr int kMaxRedirects = 3;

std::string ReadBodyCapped(Http* http) {
    std::string body;
    size_t content_length = http->GetBodyLength();
    body.reserve(content_length > 0 ? std::min(content_length, kMaxBodyBytes) : kReadChunkBytes);

    char buffer[kReadChunkBytes];
    while (body.size() < kMaxBodyBytes) {
        auto read_result = http->Read(buffer, sizeof(buffer));
        if (!read_result || *read_result <= 0) break;
        body.append(buffer, *read_result);
    }
    return body;
}

// Google Apps Script Web Apps answer both GET and POST requests to their /exec URL with a 302
// redirect to a one-off script.googleusercontent.com URL that serves the already-computed
// response; the redirect target must be fetched with GET regardless of the original method.
// esp-ml307's HttpClient (see managed_components/78__esp-ml307/src/http_client.cc) is a raw
// HTTP/1.1 client with no built-in redirect support, so this follows it by hand. This duplicates
// finance_service.cc's helper of the same shape -- see that file's comment for why it isn't
// factored out yet (not worth it until a third Apps-Script-backed tool exists).
bool OpenWithRedirects(const std::string& method, const std::string& url, const std::string& body,
                       std::unique_ptr<Http>& out_http, std::string& out_error) {
    std::string next_method = method;
    std::string next_url = url;
    bool send_body = true;

    for (int attempt = 0; attempt <= kMaxRedirects; attempt++) {
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        if (!http) {
            out_error = "Failed to create HTTP connection";
            return false;
        }
        http->SetTimeout(kHttpTimeoutMs);
        if (send_body && !body.empty()) {
            http->SetHeader("Content-Type", "application/json");
            http->SetContent(std::string(body));
        }

        auto opened = http->Open(next_method, next_url);
        if (!opened) {
            out_error = "Failed to connect to tutor sheet: " + opened.error().ToString();
            return false;
        }

        auto status_code = http->GetStatusCode();
        if (!status_code) {
            out_error = "Failed to read tutor sheet response status";
            return false;
        }

        if (*status_code >= 300 && *status_code < 400) {
            std::string location = http->GetResponseHeader("location");
            http->Close();
            if (location.empty()) {
                out_error = "Tutor sheet redirected without a Location header";
                return false;
            }
            next_method = "GET";
            next_url = location;
            send_body = false;
            continue;
        }

        out_http = std::move(http);
        return true;
    }

    out_error = "Too many redirects from tutor sheet";
    return false;
}

// Posts a JSON request to the Apps Script Web App and returns its parsed JSON response. The
// script always answers with {"ok": true, ...} or {"ok": false, "error": "..."}.
bool CallAppsScript(cJSON* request, CJsonUniquePtr& out_response, std::string& out_error) {
    CJsonStringUniquePtr request_str(cJSON_PrintUnformatted(request));
    if (!request_str) {
        out_error = "Failed to encode tutor request";
        return false;
    }

    std::unique_ptr<Http> http;
    if (!OpenWithRedirects("POST", kTutorApiUrl, request_str.get(), http, out_error)) {
        return false;
    }

    auto status_code = http->GetStatusCode();
    std::string body = ReadBodyCapped(http.get());
    http->Close();

    if (!status_code || *status_code != 200) {
        out_error = "Tutor sheet request failed with status " +
                    (status_code ? std::to_string(*status_code) : std::string("?")) +
                    (body.empty() ? "" : (": " + body));
        return false;
    }
    if (body.empty()) {
        out_error = "Tutor sheet returned an empty response";
        return false;
    }

    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    if (!root) {
        out_error = "Failed to parse tutor sheet response";
        return false;
    }

    auto ok = cJSON_GetObjectItem(root.get(), "ok");
    if (!cJSON_IsTrue(ok)) {
        auto error_item = cJSON_GetObjectItem(root.get(), "error");
        out_error =
            cJSON_IsString(error_item) ? error_item->valuestring : "Tutor sheet reported failure";
        return false;
    }

    out_response = std::move(root);
    return true;
}

std::string GetString(cJSON* obj, const char* key) {
    auto item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}

int GetInt(cJSON* obj, const char* key, int fallback) {
    auto item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

void ParseNotes(cJSON* array, std::vector<EnglishTutorNote>& out_notes) {
    out_notes.clear();
    if (!cJSON_IsArray(array)) return;
    cJSON* item;
    cJSON_ArrayForEach(item, array) {
        EnglishTutorNote note;
        note.id = GetString(item, "id");
        note.created_date = GetString(item, "created_date");
        note.category = GetString(item, "category");
        note.original = GetString(item, "original");
        note.better = GetString(item, "better");
        note.explanation = GetString(item, "explanation");
        out_notes.push_back(note);
    }
}

void ParseReviewItems(cJSON* array, std::vector<EnglishTutorReviewItem>& out_items) {
    out_items.clear();
    if (!cJSON_IsArray(array)) return;
    cJSON* item;
    cJSON_ArrayForEach(item, array) {
        EnglishTutorReviewItem review;
        review.id = GetString(item, "id");
        review.original = GetString(item, "original");
        review.better = GetString(item, "better");
        review.category = GetString(item, "category");
        review.bucket = GetString(item, "bucket");
        out_items.push_back(review);
    }
}

}  // namespace

bool EnglishTutorService::StartSession(EnglishTutorSession& out_session, std::string& out_error) {
    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate tutor request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "token", kTutorApiSecret);
    cJSON_AddStringToObject(request.get(), "action", "start_session");

    ESP_LOGI(TAG, "Starting English tutor session");

    CJsonUniquePtr response;
    if (!CallAppsScript(request.get(), response, out_error)) {
        return false;
    }

    out_session.sessions_total = GetInt(response.get(), "sessions_total", 0);
    out_session.topic = GetString(response.get(), "topic");
    out_session.prompts.clear();

    auto prompts = cJSON_GetObjectItem(response.get(), "prompts");
    if (cJSON_IsArray(prompts)) {
        cJSON* item;
        cJSON_ArrayForEach(item, prompts) {
            if (cJSON_IsString(item)) out_session.prompts.push_back(item->valuestring);
        }
    }

    ParseReviewItems(cJSON_GetObjectItem(response.get(), "due_review"), out_session.due_review);
    return true;
}

bool EnglishTutorService::LogNote(const std::string& original, const std::string& better,
                                   const std::string& explanation, const std::string& category,
                                   const std::string& bucket, std::string& out_id,
                                   std::string& out_error) {
    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate tutor request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "token", kTutorApiSecret);
    cJSON_AddStringToObject(request.get(), "action", "log_note");
    cJSON_AddStringToObject(request.get(), "original", original.c_str());
    cJSON_AddStringToObject(request.get(), "better", better.c_str());
    cJSON_AddStringToObject(request.get(), "explanation", explanation.c_str());
    cJSON_AddStringToObject(request.get(), "category", category.c_str());
    cJSON_AddStringToObject(request.get(), "bucket", bucket.c_str());

    ESP_LOGI(TAG, "Logging %s note: \"%s\" -> \"%s\" (%s)", bucket.c_str(), original.c_str(),
             better.c_str(), category.c_str());

    CJsonUniquePtr response;
    if (!CallAppsScript(request.get(), response, out_error)) {
        return false;
    }
    out_id = GetString(response.get(), "id");
    return true;
}

bool EnglishTutorService::GetDueReview(int limit, const std::string& bucket,
                                        std::vector<EnglishTutorReviewItem>& out_items,
                                        std::string& out_error) {
    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate tutor request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "token", kTutorApiSecret);
    cJSON_AddStringToObject(request.get(), "action", "get_due_review");
    cJSON_AddNumberToObject(request.get(), "limit", limit);
    cJSON_AddStringToObject(request.get(), "bucket", bucket.c_str());

    CJsonUniquePtr response;
    if (!CallAppsScript(request.get(), response, out_error)) {
        return false;
    }
    ParseReviewItems(cJSON_GetObjectItem(response.get(), "items"), out_items);
    return true;
}

bool EnglishTutorService::RecordReview(const std::string& id, bool correct,
                                        std::string& out_next_review_date,
                                        std::string& out_error) {
    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate tutor request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "token", kTutorApiSecret);
    cJSON_AddStringToObject(request.get(), "action", "record_review");
    cJSON_AddStringToObject(request.get(), "id", id.c_str());
    cJSON_AddBoolToObject(request.get(), "correct", correct);

    ESP_LOGI(TAG, "Recording review for note %s: %s", id.c_str(), correct ? "correct" : "wrong");

    CJsonUniquePtr response;
    if (!CallAppsScript(request.get(), response, out_error)) {
        return false;
    }
    out_next_review_date = GetString(response.get(), "next_review_date");
    return true;
}

bool EnglishTutorService::GetVocabWord(EnglishTutorVocabWord& out_word, std::string& out_error) {
    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate tutor request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "token", kTutorApiSecret);
    cJSON_AddStringToObject(request.get(), "action", "get_vocab_word");

    ESP_LOGI(TAG, "Fetching a vocabulary word to teach");

    CJsonUniquePtr response;
    if (!CallAppsScript(request.get(), response, out_error)) {
        return false;
    }

    out_word.word = GetString(response.get(), "word");
    out_word.meaning = GetString(response.get(), "meaning");
    out_word.examples.clear();
    auto examples = cJSON_GetObjectItem(response.get(), "examples");
    if (cJSON_IsArray(examples)) {
        cJSON* item;
        cJSON_ArrayForEach(item, examples) {
            if (cJSON_IsString(item)) out_word.examples.push_back(item->valuestring);
        }
    }
    return true;
}

bool EnglishTutorService::EndSession(const std::string& summary, std::string& out_error) {
    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate tutor request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "token", kTutorApiSecret);
    cJSON_AddStringToObject(request.get(), "action", "end_session");
    cJSON_AddStringToObject(request.get(), "summary", summary.c_str());

    ESP_LOGI(TAG, "Ending English tutor session: %s", summary.c_str());

    CJsonUniquePtr response;
    return CallAppsScript(request.get(), response, out_error);
}

bool EnglishTutorService::GetProgress(EnglishTutorProgress& out_progress,
                                       std::string& out_error) {
    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate tutor request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "token", kTutorApiSecret);
    cJSON_AddStringToObject(request.get(), "action", "get_progress");

    CJsonUniquePtr response;
    if (!CallAppsScript(request.get(), response, out_error)) {
        return false;
    }

    out_progress.sessions_total = GetInt(response.get(), "sessions_total", 0);
    out_progress.top_weakness = GetString(response.get(), "top_weakness");

    ParseNotes(cJSON_GetObjectItem(response.get(), "recent_mistakes"),
               out_progress.recent_mistakes);
    ParseNotes(cJSON_GetObjectItem(response.get(), "recent_vocabulary"),
               out_progress.recent_vocabulary);

    auto vocab_stats = cJSON_GetObjectItem(response.get(), "vocab_stats");
    out_progress.vocab_stats.in_review = GetInt(vocab_stats, "in_review", 0);
    out_progress.vocab_stats.mastered = GetInt(vocab_stats, "mastered", 0);
    return true;
}
