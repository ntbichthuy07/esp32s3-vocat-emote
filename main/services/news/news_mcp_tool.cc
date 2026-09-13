#include "news_mcp_tool.h"

#include <cJSON.h>
#include <esp_log.h>

#include "vnexpress_news_service.h"

#define TAG "NewsMcpTool"

void NewsMcpTool::Initialize() {
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddTool(
        "self.news.get_latest_vnexpress",
        "Get the latest Vietnamese news headlines from VnExpress (vnexpress.net).\n"
        "Use this when the user asks for news, tin tuc, or the latest headlines.\n"
        "Args:\n"
        "  `category`: One of `home` (trang chu / top stories), `thoi-su` (thoi su), "
        "`goc-nhin` (goc nhin), `the-gioi` (the gioi), `kinh-doanh` (kinh doanh), "
        "`bat-dong-san` (bat dong san), `khoa-hoc` (khoa hoc), `giai-tri` (giai tri), "
        "`the-thao` (the thao), `phap-luat` (phap luat), `giao-duc` (giao duc), "
        "`suc-khoe` (suc khoe), `doi-song` (doi song), `du-lich` (du lich), "
        "`so-hoa` (so hoa / cong nghe), `xe` (xe). Defaults to `home`.\n"
        "  `limit`: Maximum number of articles to return (1-10). Defaults to 5.\n"
        "Return:\n"
        "  A JSON object with the category and a list of articles, each with "
        "`title`, `url`, `description`, and `published` date.",
        PropertyList({
            Property("category", kPropertyTypeString, std::string("home")),
            Property("limit", kPropertyTypeInteger, 5, 1, 10),
        }),
        [](const PropertyList& properties) -> ToolResult {
            return HandleGetLatestNews(properties);
        });

    ESP_LOGI(TAG, "NewsMcpTool initialized");
}

ToolResult NewsMcpTool::HandleGetLatestNews(const PropertyList& properties) {
    auto category = properties["category"].value<std::string>();
    auto limit = properties["limit"].value<int>();

    std::vector<VnExpressNewsItem> items;
    std::string error;
    if (!VnExpressNewsService::FetchLatestNews(category, limit, items, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddStringToObject(root, "category", category.c_str());
    cJSON_AddNumberToObject(root, "count", static_cast<double>(items.size()));

    cJSON* articles = cJSON_CreateArray();
    if (articles == nullptr) {
        cJSON_Delete(root);
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddItemToObject(root, "articles", articles);

    for (const auto& item : items) {
        cJSON* article = cJSON_CreateObject();
        if (article == nullptr) continue;
        cJSON_AddStringToObject(article, "title", item.title.c_str());
        cJSON_AddStringToObject(article, "url", item.link.c_str());
        cJSON_AddStringToObject(article, "description", item.description.c_str());
        cJSON_AddStringToObject(article, "published", item.pub_date.c_str());
        cJSON_AddItemToArray(articles, article);
    }

    return root;
}
