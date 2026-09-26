#ifndef WEB_SEARCH_SERVICE_H
#define WEB_SEARCH_SERVICE_H

#include <string>
#include <vector>

// One web search result, as returned by Tavily.
struct WebSearchResultItem {
    std::string title;
    std::string url;
    std::string snippet;  // page content excerpt; empty url/title marks Tavily's synthesized answer
};

// Calls the Tavily search API (https://tavily.com) directly over HTTPS -- no LAN dependency on
// any other machine. Requires an API key; see kTavilyApiKey in web_search_service.cc.
class WebSearchService {
public:
    // Returns true and fills `out_items` (at most `max_results` entries) on success.
    // On failure, returns false and fills `out_error` with a human-readable reason.
    static bool Search(const std::string& query, int max_results,
                        std::vector<WebSearchResultItem>& out_items, std::string& out_error);
};

#endif  // WEB_SEARCH_SERVICE_H
