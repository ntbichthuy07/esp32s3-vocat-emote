#ifndef TOOLS_CONFIG_H
#define TOOLS_CONFIG_H

// Copy this file to service_config.h (gitignored, so your real values never get committed) and
// fill in the values whichever services you use need.

// tools/search/web_search_service.cc -- Tavily (https://tavily.com) search API. Sign up at
// tavily.com for a free-tier key (around 1000 searches/month).
constexpr const char* kTavilyApiKey = "CHANGE_ME";

// tools/finance/finance_service.cc -- Google Apps Script Web App backing the personal finance
// ledger. Deploy tools/finance/apps_script.gs as a Web App first -- see the setup steps at
// the top of that file -- then fill these in.
constexpr const char* kFinanceApiUrl =
    "https://script.google.com/macros/s/PUT_YOUR_DEPLOYMENT_ID_HERE/exec";
constexpr const char* kFinanceApiSecret = "CHANGE_ME";

#endif  // SERVICES_CONFIG_H
