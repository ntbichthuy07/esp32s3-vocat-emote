#ifndef VNEXPRESS_NEWS_SERVICE_H
#define VNEXPRESS_NEWS_SERVICE_H

#include <string>
#include <vector>

// One VnExpress article entry, as extracted from the site's public RSS feeds.
struct VnExpressNewsItem {
    std::string title;
    std::string link;
    std::string description;
    std::string pub_date;
};

// Fetches the latest headlines from vnexpress.net's public RSS feeds.
// No API key is required; VnExpress publishes these feeds for free reuse.
class VnExpressNewsService {
public:
    // Returns true and fills `out_items` (at most `limit` entries) on success.
    // On failure, returns false and fills `out_error` with a human-readable reason.
    static bool FetchLatestNews(const std::string& category, int limit,
                                 std::vector<VnExpressNewsItem>& out_items,
                                 std::string& out_error);

    // Fetches an article page and extracts its main readable text (HTML tags, scripts and
    // styles stripped). Intended for a `link` returned by FetchLatestNews, but works on any
    // vnexpress.net article URL. Returns true and fills `out_content` on success; false and
    // `out_error` otherwise (e.g. the page's markup doesn't match what we can extract).
    static bool FetchArticleDetail(const std::string& url, std::string& out_content,
                                    std::string& out_error);
};

#endif  // VNEXPRESS_NEWS_SERVICE_H
