#include "web_search_service.h"

#include <cJSON.h>
#include <esp_log.h>

#include <algorithm>

#include "board.h"
#include "cjson_utils.h"
#include "network_error.h"

#include "../service_config.h"

#define TAG "WebSearchService"

namespace {

constexpr const char* kTavilyUrl = "https://api.tavily.com/search";

constexpr int kHttpTimeoutMs = 20000;
constexpr size_t kMaxBodyBytes = 48 * 1024;
constexpr size_t kReadChunkBytes = 1024;

// Tavily's per-result `content` can run to several KB (especially with search_depth "advanced"),
// and the MCP tool result is published over MQTT as one message -- a handful of untruncated
// results was enough to overflow the transport's write and drop the connection mid-turn. Keep
// each field short: the LLM only needs enough to speak a 2-4 sentence answer, not the full page.
constexpr size_t kMaxSnippetBytes = 300;
constexpr size_t kMaxAnswerBytes = 600;

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

// Truncates to at most `max_bytes` bytes without splitting a multi-byte UTF-8 character in half
// (Vietnamese text is mostly multi-byte) -- backs off while the byte at the cut point is a UTF-8
// continuation byte (10xxxxxx).
void TruncateUtf8(std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return;
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) cut--;
    s.resize(cut);
}

}  // namespace

bool WebSearchService::Search(const std::string& query, int max_results,
                               std::vector<WebSearchResultItem>& out_items,
                               std::string& out_error) {
    out_items.clear();

    if (query.empty()) {
        out_error = "Empty search query";
        return false;
    }

    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate search request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "query", query.c_str());
    cJSON_AddNumberToObject(request.get(), "max_results", max_results);
    // "basic" (not "advanced") keeps each result's `content` to a short excerpt rather than a
    // multi-KB crawl -- see kMaxSnippetBytes above for why that matters here.
    cJSON_AddStringToObject(request.get(), "search_depth", "basic");
    cJSON_AddStringToObject(request.get(), "include_answer", "advanced");

    CJsonStringUniquePtr request_str(cJSON_PrintUnformatted(request.get()));
    if (!request_str) {
        out_error = "Failed to encode search request";
        return false;
    }

    ESP_LOGI(TAG, "Searching Tavily: %s", query.c_str());

    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http) {
        out_error = "Failed to create HTTP connection";
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Authorization", std::string("Bearer ") + kTavilyApiKey);
    http->SetContent(std::string(request_str.get()));

    auto opened = http->Open("POST", kTavilyUrl);
    if (!opened) {
        out_error = "Failed to connect to Tavily: " + opened.error().ToString();
        return false;
    }

    auto status_code = http->GetStatusCode();
    if (!status_code) {
        http->Close();
        out_error = "Failed to read Tavily response status";
        return false;
    }
    if (*status_code != 200) {
        std::string body = ReadBodyCapped(http.get());
        http->Close();
        out_error =
            "Tavily search failed with status " + std::to_string(*status_code) +
            (body.empty() ? "" : (": " + body));
        return false;
    }

    std::string body = ReadBodyCapped(http.get());
    http->Close();

    if (body.empty()) {
        out_error = "Tavily returned an empty response";
        return false;
    }

    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    if (!root) {
        out_error = "Failed to parse Tavily response";
        return false;
    }

    auto error_item = cJSON_GetObjectItem(root.get(), "error");
    if (cJSON_IsString(error_item) && error_item->valuestring[0] != '\0') {
        out_error = std::string("Tavily error: ") + error_item->valuestring;
        return false;
    }

    // Tavily's synthesized answer, when present, is the single most useful thing to read back --
    // surfaced as its own pseudo-result (empty url) so callers see it without special-casing.
    auto answer = cJSON_GetObjectItem(root.get(), "answer");
    if (cJSON_IsString(answer) && answer->valuestring[0] != '\0') {
        WebSearchResultItem summary;
        summary.title = "Summary";
        summary.snippet = answer->valuestring;
        TruncateUtf8(summary.snippet, kMaxAnswerBytes);
        out_items.push_back(std::move(summary));
    }

    auto results = cJSON_GetObjectItem(root.get(), "results");
    if (cJSON_IsArray(results)) {
        cJSON* entry;
        cJSON_ArrayForEach(entry, results) {
            if (static_cast<int>(out_items.size()) >= max_results + 1) break;  // +1 for the summary

            auto title = cJSON_GetObjectItem(entry, "title");
            auto url_item = cJSON_GetObjectItem(entry, "url");
            auto content_item = cJSON_GetObjectItem(entry, "content");

            WebSearchResultItem item;
            item.title = cJSON_IsString(title) ? title->valuestring : "";
            item.url = cJSON_IsString(url_item) ? url_item->valuestring : "";
            item.snippet = cJSON_IsString(content_item) ? content_item->valuestring : "";
            TruncateUtf8(item.snippet, kMaxSnippetBytes);

            if (item.title.empty() && item.snippet.empty()) continue;
            out_items.push_back(std::move(item));
        }
    }

    if (out_items.empty()) {
        out_error = "No search results found for: " + query;
        return false;
    }

    ESP_LOGI(TAG, "Got %d search results for '%s'", static_cast<int>(out_items.size()),
             query.c_str());
    return true;
}
