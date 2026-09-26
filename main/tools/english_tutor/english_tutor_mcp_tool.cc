#include "english_tutor_mcp_tool.h"

#include <cJSON.h>
#include <esp_log.h>

#include "english_tutor_service.h"

#define TAG "EnglishTutorMcpTool"

namespace {

constexpr const char* kLevelHelp =
    "1 Beginner, 2 Elementary, 3 Pre-intermediate, 4 Intermediate, 5 Upper-intermediate, "
    "6 Conversation, 7 Work, 8 Discussion, 9 Advanced, 10 Fluent.";

}  // namespace

void EnglishTutorMcpTool::Initialize() {
    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddTool(
        "self.tutor.start_session",
        std::string(
            "Starts a 1-on-1 English practice session. Call this the moment the learner asks "
            "to practice English (e.g. 'Let's practice English', 'I want to learn English', "
            "'Luyen tieng Anh'). Returns the learner's current level and a topic to open the "
            "conversation with -- speak naturally using the returned topic/prompts instead of "
            "asking the learner what they want to talk about.\n"
            "Return:\n"
            "  `level`: integer 1-10. `level_name`: ") +
            kLevelHelp +
            "\n"
            "  `sessions_total`: how many practice sessions the learner has completed so far.\n"
            "  `topic`: the subject to open with.\n"
            "  `prompts`: one or more example opening questions for that topic.",
        PropertyList(),
        [](const PropertyList& properties) -> ToolResult {
            return HandleStartSession(properties);
        });

    mcp_server.AddTool(
        "self.tutor.log_mistake",
        "Silently records one grammar or meaning mistake you corrected during the practice "
        "conversation, so it can be reviewed later -- never mention this call to the learner. "
        "Call it right after you correct an important mistake in your spoken reply.\n"
        "Args:\n"
        "  `original`: exactly what the learner said.\n"
        "  `better`: your corrected version of it.\n"
        "  `explanation`: optional short reason for the fix, e.g. 'past tense for a completed "
        "action'.\n"
        "  `category`: optional short label to group similar mistakes, e.g. 'past_tense', "
        "'prepositions', 'articles'.\n"
        "Return:\n"
        "  A JSON object confirming it was recorded.",
        PropertyList({
            Property("original", kPropertyTypeString),
            Property("better", kPropertyTypeString),
            Property("explanation", kPropertyTypeString, std::string("")),
            Property("category", kPropertyTypeString, std::string("")),
        }),
        [](const PropertyList& properties) -> ToolResult {
            return HandleLogMistake(properties);
        });

    mcp_server.AddTool(
        "self.tutor.set_level",
        std::string(
            "Changes the learner's stored practice level. Only call this when the learner "
            "explicitly asks to change level, or when you suggest it yourself and the learner "
            "agrees -- never silently.\n"
            "Args:\n"
            "  `level`: the new level, 1-10: ") +
            kLevelHelp +
            "\n"
            "Return:\n"
            "  Confirmation with the new level's name.",
        PropertyList({
            Property("level", kPropertyTypeInteger, 1, 10),
        }),
        [](const PropertyList& properties) -> ToolResult { return HandleSetLevel(properties); });

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
        "Fetches the learner's current level and most recently logged mistakes. Call this if "
        "the learner asks how they're doing, or if you want to review recent mistakes before "
        "quizzing them again.\n"
        "Return:\n"
        "  `level`, `level_name`, `sessions_total`, and `recent_mistakes` (up to 5, most "
        "recent first, each with `original`/`better`/`explanation`/`category`).",
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
    cJSON_AddNumberToObject(root, "level", session.level);
    cJSON_AddStringToObject(root, "level_name", session.level_name.c_str());
    cJSON_AddNumberToObject(root, "sessions_total", session.sessions_total);
    cJSON_AddStringToObject(root, "topic", session.topic.c_str());

    cJSON* prompts = cJSON_CreateArray();
    if (prompts != nullptr) {
        cJSON_AddItemToObject(root, "prompts", prompts);
        for (const auto& prompt : session.prompts) {
            cJSON_AddItemToArray(prompts, cJSON_CreateString(prompt.c_str()));
        }
    }
    return root;
}

ToolResult EnglishTutorMcpTool::HandleLogMistake(const PropertyList& properties) {
    auto original = properties["original"].value<std::string>();
    auto better = properties["better"].value<std::string>();
    auto explanation = properties["explanation"].value<std::string>();
    auto category = properties["category"].value<std::string>();

    std::string id, error;
    if (!EnglishTutorService::LogMistake(original, better, explanation, category, id, error)) {
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

ToolResult EnglishTutorMcpTool::HandleSetLevel(const PropertyList& properties) {
    auto level = properties["level"].value<int>();

    std::string level_name, error;
    if (!EnglishTutorService::SetLevel(level, level_name, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddNumberToObject(root, "level", level);
    cJSON_AddStringToObject(root, "level_name", level_name.c_str());
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
    cJSON_AddNumberToObject(root, "level", progress.level);
    cJSON_AddStringToObject(root, "level_name", progress.level_name.c_str());
    cJSON_AddNumberToObject(root, "sessions_total", progress.sessions_total);

    cJSON* mistakes = cJSON_CreateArray();
    if (mistakes != nullptr) {
        cJSON_AddItemToObject(root, "recent_mistakes", mistakes);
        for (const auto& mistake : progress.recent_mistakes) {
            cJSON* item = cJSON_CreateObject();
            if (item == nullptr) continue;
            cJSON_AddStringToObject(item, "id", mistake.id.c_str());
            cJSON_AddStringToObject(item, "created_date", mistake.created_date.c_str());
            cJSON_AddStringToObject(item, "category", mistake.category.c_str());
            cJSON_AddStringToObject(item, "original", mistake.original.c_str());
            cJSON_AddStringToObject(item, "better", mistake.better.c_str());
            cJSON_AddStringToObject(item, "explanation", mistake.explanation.c_str());
            cJSON_AddItemToArray(mistakes, item);
        }
    }
    return root;
}
