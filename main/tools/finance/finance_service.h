#ifndef FINANCE_SERVICE_H
#define FINANCE_SERVICE_H

#include <string>
#include <vector>


// One recorded transaction, as returned by FinanceService::ListTransactions.
struct FinanceTransaction {
    std::string id;
    std::string date;  // "YYYY-MM-DDTHH:MM:SS"
    long long amount = 0;  // VND; positive income, negative expense (like a bank statement)
    std::string category;
    std::string description;
};

struct FinanceTransactionList {
    std::string period;  // empty if the list wasn't filtered to a period
    std::vector<FinanceTransaction> transactions;
};

struct FinanceAddResult {
    std::string id;
    // As normalized by the Apps Script (see ALLOWED_CATEGORIES in apps_script.gs) -- may differ
    // from the category that was requested if that one wasn't recognized. The sign of `amount`
    // may also have been corrected to match whether the category is an income or expense one.
    std::string category;
    int amount = 0;
};


struct FinanceCategoryMonthly {
    std::string category;
    long long amount = 0;  // VND, from column H (CurrentMonthly)
};

// Current month's financial metrics and category spending directly from columns F and H.
// H2 = income, H3 = remaining (saving), spent = H2 - H3.
struct FinanceMonthlySpending {
    long long income = 0;     // VND, H2
    long long remaining = 0;  // VND, H3
    long long spent = 0;      // VND, H2 - H3
    std::string category;
    bool category_found = false;
    long long category_amount = 0;  // VND, for requested category from H4 onwards
    std::vector<FinanceCategoryMonthly> categories;
};




// Talks to a Google Apps Script Web App backed by a Google Sheet that acts as the transaction
// ledger. Deploy apps_script.gs (in this directory) as a Web App and fill in
// tools/service_config.h (copy it from service_config.example.h) before using any method
// below.
class FinanceService {
public:
    // Appends one transaction. `date` is "today" (default when empty), "yesterday",
    // "day_before_yesterday", or an explicit "YYYY-MM-DD"; "today" uses the device's current
    // time of day, other values use noon on that date since the real time isn't known. Returns
    // true and fills out_result on success; false and out_error otherwise.
    static bool AddTransaction(int amount, const std::string& category,
                                const std::string& description, const std::string& date,
                                FinanceAddResult& out_result, std::string& out_error);

    // Replaces the amount/category/description of the transaction with the given id (its date
    // is left unchanged). Returns true on success; false and out_error otherwise (e.g. not
    // found).
    static bool UpdateTransaction(const std::string& id, int amount, const std::string& category,
                                   const std::string& description, std::string& out_error);

    // Deletes the transaction with the given id, as previously returned by AddTransaction or
    // ListTransactions. Returns true on success; false and out_error otherwise (e.g. not found).
    static bool DeleteTransaction(const std::string& id, std::string& out_error);

    // Fetches up to `limit` transactions, most recent first, optionally filtered to `period`
    // ("today", "yesterday", "week", "month", "YYYY-MM", or "YYYY-MM-DD"); an empty `period`
    // returns transactions from any time. Returns true and fills out_list on success; false and
    // out_error otherwise.
    static bool ListTransactions(const std::string& period, int limit,
                                  FinanceTransactionList& out_list, std::string& out_error);


    // Fetches current month's spending and balance directly from spreadsheet columns F and H.
    // H2 = income, H3 = remaining (saving), spent = H2 - H3. If `category` is non-empty,
    // looks up that category's spending from H4 onwards. Returns true on success; false and out_error otherwise.
    static bool GetMonthlySpending(const std::string& category,
                                   FinanceMonthlySpending& out_result,
                                   std::string& out_error);


};

#endif  // FINANCE_SERVICE_H
