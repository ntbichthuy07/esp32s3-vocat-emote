#include "web_search_mcp_tool.h"

#include <cJSON.h>
#include <esp_log.h>

#include "web_search_service.h"

#define TAG "WebSearchMcpTool"

void WebSearchMcpTool::Initialize() {
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddTool(
        "self.search.web_search",
        "Search the web (like a Google search, backed by Tavily) for information that needs to be "
        "current or isn't otherwise available. Prefer calling this tool whenever the answer could be "
        "stale or unknown: latest news, prices/specs of products, exchange rates, upcoming events, "
        "facts about people/companies, comparisons between multiple things, trending/viral social "
        "media topics (Threads/TikTok/Facebook), or currently popular movies. Prefer reputable/"
        "official and recent sources, state how current the info is (e.g. a price's date), and never "
        "invent facts, engagement numbers, or 'viral' status not backed by the results. If the user "
        "explicitly asks to search via Tavily, always call this tool regardless of the above.\n"
        "Args:\n"
        "  `query`: The search query, phrased as you would type into a search engine.\n"
        "  `limit`: Maximum number of results to return (1-5). Defaults to 4.\n"
        "Return:\n"
        "  A JSON object with the query and a list of results, each with `title`, `url`, and "
        "`snippet` (a short excerpt of the page content).",
        PropertyList({
            Property("query", kPropertyTypeString),
            Property("limit", kPropertyTypeInteger, 4, 1, 5),
        }),
        [](const PropertyList& properties) -> ToolResult { return HandleSearch(properties); });

    ESP_LOGI(TAG, "WebSearchMcpTool initialized");
}

ToolResult WebSearchMcpTool::HandleSearch(const PropertyList& properties) {
    auto query = properties["query"].value<std::string>();
    auto limit = properties["limit"].value<int>();

    std::vector<WebSearchResultItem> items;
    std::string error;
    if (!WebSearchService::Search(query, limit, items, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddStringToObject(root, "query", query.c_str());
    cJSON_AddNumberToObject(root, "count", static_cast<double>(items.size()));

    cJSON* results = cJSON_CreateArray();
    if (results == nullptr) {
        cJSON_Delete(root);
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddItemToObject(root, "results", results);

    for (const auto& item : items) {
        cJSON* result = cJSON_CreateObject();
        if (result == nullptr) continue;
        cJSON_AddStringToObject(result, "title", item.title.c_str());
        cJSON_AddStringToObject(result, "url", item.url.c_str());
        cJSON_AddStringToObject(result, "snippet", item.snippet.c_str());
        cJSON_AddItemToArray(results, result);
    }

    return root;
}
