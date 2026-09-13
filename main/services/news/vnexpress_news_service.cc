#include "vnexpress_news_service.h"

#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <unordered_map>

#include "board.h"
#include "network_error.h"

#define TAG "VnExpressNewsService"

namespace {

constexpr int kHttpTimeoutMs = 10000;
// VnExpress RSS feeds run a few hundred KB on "trang chu"; cap what we buffer
// in RAM since embedded boards don't need (and can't afford) the whole thing.
constexpr size_t kMaxBodyBytes = 32 * 1024;
constexpr size_t kReadChunkBytes = 1024;

// Maps the user-facing category name (matching vnexpress.net/<category> paths)
// to the RSS feed slug under https://vnexpress.net/rss/<slug>.rss
const std::unordered_map<std::string, std::string>& CategorySlugs() {
    static const std::unordered_map<std::string, std::string> kSlugs = {
        {"home", "tin-moi-nhat"},
        {"thoi-su", "thoi-su"},
        {"goc-nhin", "goc-nhin"},
        {"the-gioi", "the-gioi"},
        {"kinh-doanh", "kinh-doanh"},
        {"bat-dong-san", "bat-dong-san"},
        {"khoa-hoc", "khoa-hoc"},
        {"giai-tri", "giai-tri"},
        {"the-thao", "the-thao"},
        {"phap-luat", "phap-luat"},
        {"giao-duc", "giao-duc"},
        {"suc-khoe", "suc-khoe"},
        {"doi-song", "doi-song"},
        {"du-lich", "du-lich"},
        {"so-hoa", "so-hoa"},
        {"xe", "oto-xe-may"},
    };
    return kSlugs;
}

std::string Trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
    return s.substr(start, end - start);
}

// Extracts the text content of <tag>...</tag> within `block`, unwrapping a
// <![CDATA[...]]> section if present. Returns "" if the tag is not found.
std::string ExtractXmlTag(const std::string& block, const std::string& tag) {
    const std::string open_tag = "<" + tag + ">";
    const std::string close_tag = "</" + tag + ">";

    size_t start = block.find(open_tag);
    if (start == std::string::npos) return "";
    start += open_tag.size();
    size_t end = block.find(close_tag, start);
    if (end == std::string::npos) return "";

    std::string content = block.substr(start, end - start);

    const std::string cdata_open = "<![CDATA[";
    const std::string cdata_close = "]]>";
    size_t cdata_start = content.find(cdata_open);
    if (cdata_start != std::string::npos) {
        size_t cdata_end = content.find(cdata_close, cdata_start + cdata_open.size());
        if (cdata_end != std::string::npos) {
            content = content.substr(cdata_start + cdata_open.size(),
                                      cdata_end - (cdata_start + cdata_open.size()));
        }
    }

    return Trim(content);
}

// Reads at most kMaxBodyBytes of the HTTP response body.
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

}  // namespace

bool VnExpressNewsService::FetchLatestNews(const std::string& category, int limit,
                                            std::vector<VnExpressNewsItem>& out_items,
                                            std::string& out_error) {
    out_items.clear();

    const auto& slugs = CategorySlugs();
    auto slug_it = slugs.find(category);
    if (slug_it == slugs.end()) {
        out_error = "Unknown VnExpress category: " + category;
        return false;
    }

    std::string url = "https://vnexpress.net/rss/" + slug_it->second + ".rss";
    ESP_LOGI(TAG, "Fetching VnExpress RSS: %s", url.c_str());

    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http) {
        out_error = "Failed to create HTTP connection";
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Accept", "application/rss+xml, text/xml, */*");

    auto opened = http->Open("GET", url);
    if (!opened) {
        out_error = "Failed to connect to VnExpress: " + opened.error().ToString();
        return false;
    }

    auto status_code = http->GetStatusCode();
    if (!status_code || *status_code != 200) {
        int code = status_code ? *status_code : -1;
        http->Close();
        out_error = "VnExpress RSS request failed with status " + std::to_string(code);
        return false;
    }

    std::string body = ReadBodyCapped(http.get());
    http->Close();

    if (body.empty()) {
        out_error = "VnExpress RSS response was empty";
        return false;
    }

    // Walk each <item>...</item> block and pull out the fields we care about.
    size_t search_pos = 0;
    while (static_cast<int>(out_items.size()) < limit) {
        size_t item_start = body.find("<item>", search_pos);
        if (item_start == std::string::npos) break;
        size_t item_end = body.find("</item>", item_start);
        if (item_end == std::string::npos) break;

        std::string item_block = body.substr(item_start, item_end - item_start);
        search_pos = item_end + 7;  // length of "</item>"

        VnExpressNewsItem item;
        item.title = ExtractXmlTag(item_block, "title");
        item.link = ExtractXmlTag(item_block, "link");
        item.description = ExtractXmlTag(item_block, "description");
        item.pub_date = ExtractXmlTag(item_block, "pubDate");

        if (item.title.empty() || item.link.empty()) continue;
        out_items.push_back(std::move(item));
    }

    if (out_items.empty()) {
        out_error = "No articles found in VnExpress RSS feed";
        return false;
    }

    ESP_LOGI(TAG, "Parsed %d VnExpress articles for category '%s'",
             static_cast<int>(out_items.size()), category.c_str());
    return true;
}
