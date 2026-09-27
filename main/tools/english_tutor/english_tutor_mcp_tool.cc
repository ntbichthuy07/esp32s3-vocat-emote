#include "english_tutor_mcp_tool.h"

#include <cJSON.h>
#include <esp_log.h>

#include "english_tutor_service.h"

#define TAG "EnglishTutorMcpTool"

namespace {

constexpr const char* kBucketHelp =
    "`weakness` for a grammar/meaning correction, `vocabulary` for a native/idiomatic "
    "expression you handed over (even if what the learner said wasn't wrong).";

cJSON* ReviewItemsToJson(const std::vector<EnglishTutorReviewItem>& items) {
    cJSON* array = cJSON_CreateArray();
    if (array == nullptr) return nullptr;
    for (const auto& item : items) {
        cJSON* obj = cJSON_CreateObject();
        if (obj == nullptr) continue;
        cJSON_AddStringToObject(obj, "id", item.id.c_str());
        cJSON_AddStringToObject(obj, "original", item.original.c_str());
        cJSON_AddStringToObject(obj, "better", item.better.c_str());
        cJSON_AddStringToObject(obj, "category", item.category.c_str());
        cJSON_AddStringToObject(obj, "bucket", item.bucket.c_str());
        cJSON_AddItemToArray(array, obj);
    }
    return array;
}

cJSON* NotesToJson(const std::vector<EnglishTutorNote>& notes) {
    cJSON* array = cJSON_CreateArray();
    if (array == nullptr) return nullptr;
    for (const auto& note : notes) {
        cJSON* obj = cJSON_CreateObject();
        if (obj == nullptr) continue;
        cJSON_AddStringToObject(obj, "id", note.id.c_str());
        cJSON_AddStringToObject(obj, "created_date", note.created_date.c_str());
        cJSON_AddStringToObject(obj, "category", note.category.c_str());
        cJSON_AddStringToObject(obj, "original", note.original.c_str());
        cJSON_AddStringToObject(obj, "better", note.better.c_str());
        cJSON_AddStringToObject(obj, "explanation", note.explanation.c_str());
        cJSON_AddItemToArray(array, obj);
    }
    return array;
}

}  // namespace

void EnglishTutorMcpTool::Initialize() {
    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddTool(
        "self.tutor.start_session",
        "Starts a 1-on-1 English practice session. Call this the moment the learner asks to "
        "practice English (e.g. 'Let's practice English', 'I want to learn English', 'Luyen "
        "tieng Anh'). Returns a topic to open the conversation with (already biased toward "
        "their recent recurring weakness, if any), and up to 2 old mistakes/expressions due "
        "for review -- speak naturally using the returned topic/prompts instead of asking the "
        "learner what they want to talk about; optionally warm up with one due-review item "
        "before moving to the topic, but don't do this every single session.\n"
        "Return:\n"
        "  `sessions_total`: how many practice sessions the learner has completed so far.\n"
        "  `topic`: the subject to open with.\n"
        "  `prompts`: one or more example opening questions for that topic.\n"
        "  `due_review`: 0-2 items `{id, original, better, category, bucket}` due for review "
        "today.",
        PropertyList(),
        [](const PropertyList& properties) -> ToolResult {
            return HandleStartSession(properties);
        });

    mcp_server.AddTool(
        "self.tutor.log_note",
        std::string(
            "Silently records one correction or useful expression from the practice "
            "conversation, so it can be reviewed later -- never mention this call to the "
            "learner. Call it right after you correct an important mistake, or right after "
            "you offer a more natural/native way to say something (even if what the learner "
            "said wasn't wrong).\n"
            "Args:\n"
            "  `original`: exactly what the learner said (leave empty for a pure vocabulary "
            "moment where nothing was wrong).\n"
            "  `better`: your corrected version, or the more natural/native phrasing.\n"
            "  `explanation`: optional short reason, e.g. 'past tense for a completed "
            "action'.\n"
            "  `category`: optional short label to group similar notes, e.g. 'past_tense', "
            "'prepositions', 'idiom', 'phrasal_verb'.\n"
            "  `bucket`: ") +
            kBucketHelp +
            "\n"
            "Return:\n"
            "  A JSON object confirming it was recorded.",
        PropertyList({
            Property("original", kPropertyTypeString, std::string("")),
            Property("better", kPropertyTypeString),
            Property("explanation", kPropertyTypeString, std::string("")),
            Property("category", kPropertyTypeString, std::string("")),
            Property("bucket", kPropertyTypeString, std::string("weakness")),
        }),
        [](const PropertyList& properties) -> ToolResult { return HandleLogNote(properties); });

    mcp_server.AddTool(
        "self.tutor.get_due_review",
        "Fetches specific past mistakes/expressions due for review today (oldest-due first), "
        "to actually quiz the learner on -- e.g. 'Earlier you said ... -- do you remember the "
        "better way to say that?'. Use this to warm up occasionally, not as a rigid quiz every "
        "session.\n"
        "Args:\n"
        "  `limit`: maximum items to return (1-10). Defaults to 5.\n"
        "  `bucket`: optional filter, `weakness` or `vocabulary`; leave empty for both.\n"
        "Return:\n"
        "  `items`: an array of `{id, original, better, category, bucket}`, oldest-due first.",
        PropertyList({
            Property("limit", kPropertyTypeInteger, 5, 1, 10),
            Property("bucket", kPropertyTypeString, std::string("")),
        }),
        [](const PropertyList& properties) -> ToolResult {
            return HandleGetDueReview(properties);
        });

    mcp_server.AddTool(
        "self.tutor.record_review",
        "Updates spaced-repetition scheduling for one item after quizzing the learner on it "
        "via self.tutor.get_due_review or self.tutor.start_session's `due_review`. Call this "
        "right after the learner attempts to recall/correct it, whether or not they got it "
        "right.\n"
        "Args:\n"
        "  `id`: the item's id.\n"
        "  `correct`: whether the learner got it right this time.\n"
        "Return:\n"
        "  Confirmation with the next review date.",
        PropertyList({
            Property("id", kPropertyTypeString),
            Property("correct", kPropertyTypeBoolean, false),
        }),
        [](const PropertyList& properties) -> ToolResult {
            return HandleRecordReview(properties);
        });

    mcp_server.AddTool(
        "self.tutor.get_vocab_word",
        "Fetches one curated vocabulary word to actively teach the learner, complete with its "
        "meaning and a couple of example sentences -- use this occasionally to introduce new "
        "vocabulary mid-conversation, not every session. Weave the word and one example "
        "naturally into what you say next; don't just read the definition out loud. This is "
        "separate from self.tutor.log_note(bucket=\"vocabulary\"), which is for expressions "
        "that come up naturally in the conversation rather than ones you deliberately teach.\n"
        "Return:\n"
        "  `word`, `meaning`, `examples` (an array of example sentences).",
        PropertyList(),
        [](const PropertyList& properties) -> ToolResult {
            return HandleGetVocabWord(properties);
        });

    mcp_server.AddTool(
        "self.tutor.end_session",
        "Closes the current practice session. Call this when the learner wants to stop "
        "practicing. Before calling, verbally summarize 2-3 important mistakes and a few "
        "useful expressions from this conversation for the learner.\n"
        "Args:\n"
        "  `summary`: a short note of what was practiced this session (topics covered, main "
        "mistake patterns), for the learner's own history.\n"
        "Return:\n"
        "  Confirmation.",
        PropertyList({
            Property("summary", kPropertyTypeString, std::string("")),
        }),
        [](const PropertyList& properties) -> ToolResult {
            return HandleEndSession(properties);
        });

    mcp_server.AddTool(
        "self.tutor.get_progress",
        "Fetches the learner's top recurring weakness, vocabulary stats, and most recently "
        "logged mistakes/expressions. Call this if the learner asks how they're doing.\n"
        "Return:\n"
        "  `sessions_total`, `top_weakness` (may be empty), `vocab_stats` (`{in_review, "
        "mastered}`), `recent_mistakes` and `recent_vocabulary` (up to 5 each, most recent "
        "first, each with `original`/`better`/`explanation`/`category`).",
        PropertyList(),
        [](const PropertyList& properties) -> ToolResult {
            return HandleGetProgress(properties);
        });

    ESP_LOGI(TAG, "EnglishTutorMcpTool initialized");
}

ToolResult EnglishTutorMcpTool::HandleStartSession(const PropertyList& properties) {
    EnglishTutorSession session;
    std::string error;
    if (!EnglishTutorService::StartSession(session, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddNumberToObject(root, "sessions_total", session.sessions_total);
    cJSON_AddStringToObject(root, "topic", session.topic.c_str());

    cJSON* prompts = cJSON_CreateArray();
    if (prompts != nullptr) {
        cJSON_AddItemToObject(root, "prompts", prompts);
        for (const auto& prompt : session.prompts) {
            cJSON_AddItemToArray(prompts, cJSON_CreateString(prompt.c_str()));
        }
    }

    cJSON* due_review = ReviewItemsToJson(session.due_review);
    if (due_review != nullptr) cJSON_AddItemToObject(root, "due_review", due_review);
    return root;
}

ToolResult EnglishTutorMcpTool::HandleLogNote(const PropertyList& properties) {
    auto original = properties["original"].value<std::string>();
    auto better = properties["better"].value<std::string>();
    auto explanation = properties["explanation"].value<std::string>();
    auto category = properties["category"].value<std::string>();
    auto bucket = properties["bucket"].value<std::string>();

    std::string id, error;
    if (!EnglishTutorService::LogNote(original, better, explanation, category, bucket, id,
                                       error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddBoolToObject(root, "recorded", true);
    cJSON_AddStringToObject(root, "id", id.c_str());
    return root;
}

ToolResult EnglishTutorMcpTool::HandleGetDueReview(const PropertyList& properties) {
    auto limit = properties["limit"].value<int>();
    auto bucket = properties["bucket"].value<std::string>();

    std::vector<EnglishTutorReviewItem> items;
    std::string error;
    if (!EnglishTutorService::GetDueReview(limit, bucket, items, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON* array = ReviewItemsToJson(items);
    if (array != nullptr) cJSON_AddItemToObject(root, "items", array);
    return root;
}

ToolResult EnglishTutorMcpTool::HandleRecordReview(const PropertyList& properties) {
    auto id = properties["id"].value<std::string>();
    auto correct = properties["correct"].value<bool>();

    std::string next_review_date, error;
    if (!EnglishTutorService::RecordReview(id, correct, next_review_date, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddBoolToObject(root, "recorded", true);
    cJSON_AddStringToObject(root, "next_review_date", next_review_date.c_str());
    return root;
}

ToolResult EnglishTutorMcpTool::HandleGetVocabWord(const PropertyList& properties) {
    EnglishTutorVocabWord word;
    std::string error;
    if (!EnglishTutorService::GetVocabWord(word, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddStringToObject(root, "word", word.word.c_str());
    cJSON_AddStringToObject(root, "meaning", word.meaning.c_str());

    cJSON* examples = cJSON_CreateArray();
    if (examples != nullptr) {
        cJSON_AddItemToObject(root, "examples", examples);
        for (const auto& example : word.examples) {
            cJSON_AddItemToArray(examples, cJSON_CreateString(example.c_str()));
        }
    }
    return root;
}

ToolResult EnglishTutorMcpTool::HandleEndSession(const PropertyList& properties) {
    auto summary = properties["summary"].value<std::string>();

    std::string error;
    if (!EnglishTutorService::EndSession(summary, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddBoolToObject(root, "ended", true);
    return root;
}

ToolResult EnglishTutorMcpTool::HandleGetProgress(const PropertyList& properties) {
    EnglishTutorProgress progress;
    std::string error;
    if (!EnglishTutorService::GetProgress(progress, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddNumberToObject(root, "sessions_total", progress.sessions_total);
    cJSON_AddStringToObject(root, "top_weakness", progress.top_weakness.c_str());

    cJSON* vocab_stats = cJSON_CreateObject();
    if (vocab_stats != nullptr) {
        cJSON_AddItemToObject(root, "vocab_stats", vocab_stats);
        cJSON_AddNumberToObject(vocab_stats, "in_review", progress.vocab_stats.in_review);
        cJSON_AddNumberToObject(vocab_stats, "mastered", progress.vocab_stats.mastered);
    }

    cJSON* recent_mistakes = NotesToJson(progress.recent_mistakes);
    if (recent_mistakes != nullptr) {
        cJSON_AddItemToObject(root, "recent_mistakes", recent_mistakes);
    }

    cJSON* recent_vocabulary = NotesToJson(progress.recent_vocabulary);
    if (recent_vocabulary != nullptr) {
        cJSON_AddItemToObject(root, "recent_vocabulary", recent_vocabulary);
    }
    return root;
}
