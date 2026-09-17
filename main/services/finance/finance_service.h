#ifndef FINANCE_SERVICE_H
#define FINANCE_SERVICE_H

#include <string>
#include <vector>

// A resolved reporting window: "today", "yesterday", "week", "month" (default), an explicit
// "YYYY-MM", or an explicit "YYYY-MM-DD" -- see FinanceService's period-taking methods.
// `income`/`expense`/`net`/`count` cover exactly the transactions dated within that window, as
// pre-aggregated by the Apps Script (not on-device) so the MCP result stays tiny regardless of
// how many transactions the sheet holds.
struct FinanceSummary {
    std::string period;  // human-readable label, e.g. "2026-09" or "2026-09-17"
    long long income = 0;   // VND, sum of income transactions (positive)
    long long expense = 0;  // VND, sum of expense transactions (positive)
    long long net = 0;      // VND, income - expense
    int count = 0;
};

// One category's pre-aggregated total for a period. `total` keeps the sign transactions were
// stored with (positive for income categories, negative for expense categories).
struct FinanceCategoryTotal {
    std::string category;
    long long total = 0;  // VND
    int count = 0;
};

struct FinanceCategorySummary {
    std::string period;
    std::vector<FinanceCategoryTotal> categories;  // sorted by |total| descending
};

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

// A category's monthly budget status, as computed by the Apps Script from the budget columns
// (F:G on the Transactions sheet) plus that category's spending for the period.
struct FinanceBudgetStatus {
    std::string category;
    long long budget = 0;     // VND, as configured via SetBudget
    long long spent = 0;      // VND
    long long remaining = 0;  // VND, budget - spent (can be negative if exceeded)
    double percentage = 0;    // spent / budget * 100
    std::string status;       // "ok" (<80%), "warning" (>=80%), or "exceeded" (>=100%)
};

// A funding goal's progress, as computed by the Apps Script from the goal columns (J:P on the
// Transactions sheet). `completed` is derived (saved >= target), not stored. `saved` and
// `completed_date` are filled in directly on the sheet as money is set aside / a goal is
// finished -- no tool updates them. `id` (e.g. "F1") is what self.finance.add_transaction's
// `description` should reference to link a transaction back to this goal -- see
// FUNDING_GOAL_SAVED in apps_script.gs, which matches on this id rather than on `name`.
struct FinanceFundingGoal {
    std::string id;
    std::string name;
    long long target = 0;      // VND
    long long saved = 0;       // VND
    long long remaining = 0;   // VND, target - saved
    double percentage = 0;     // saved / target * 100
    bool completed = false;
    std::string deadline;        // "YYYY-MM-DD", or empty if none was set
    std::string completed_date;  // "YYYY-MM-DD", or empty if not filled in yet
};

struct FinanceFundingGoalList {
    std::vector<FinanceFundingGoal> goals;
};

// The result of comparing one metric (a category's total, or overall expense if `category` is
// empty) between two periods.
struct FinancePeriodComparison {
    std::string period_a;
    std::string period_b;
    std::string category;    // empty if comparing overall expense rather than one category
    long long value_a = 0;    // VND
    long long value_b = 0;    // VND
    long long difference = 0;   // value_a - value_b
    double percent_change = 0;  // relative to value_b; 0 if value_b is 0
};

// Talks to a Google Apps Script Web App backed by a Google Sheet that acts as the transaction
// ledger. Deploy apps_script.gs (in this directory) as a Web App and fill in
// services/service_config.h (copy it from service_config.example.h) before using any method
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

    // Fetches the income/expense/net totals for `period` ("today", "yesterday", "week", "month",
    // "YYYY-MM", or "YYYY-MM-DD"); an empty `period` defaults to "month". Returns true and fills
    // out_summary on success; false and out_error otherwise.
    static bool GetSummary(const std::string& period, FinanceSummary& out_summary,
                            std::string& out_error);

    // Fetches per-category totals for `period` (same accepted values as GetSummary), sorted by
    // magnitude descending. Returns true and fills out_summary on success; false and out_error
    // otherwise.
    static bool GetCategorySummary(const std::string& period, FinanceCategorySummary& out_summary,
                                    std::string& out_error);

    // Sets (or replaces) a category's recurring monthly budget limit. Pass `limit` <= 0 to clear
    // it. Returns true on success; false and out_error otherwise.
    static bool SetBudget(const std::string& category, long long limit, std::string& out_error);

    // Fetches a category's budget status for `period` (same accepted values as GetSummary; an
    // empty `period` defaults to "month", matching the recurring monthly budget). Returns true
    // and fills out_status on success; false and out_error otherwise (e.g. no budget set for
    // that category).
    static bool GetBudgetStatus(const std::string& category, const std::string& period,
                                 FinanceBudgetStatus& out_status, std::string& out_error);

    // Compares one metric between two periods: if `category` is empty, compares overall expense
    // totals; otherwise compares that category's total. `period_b` defaults to the period
    // immediately before `period_a` (the previous day/week/month, matching period_a's kind) when
    // left empty. Returns true and fills out_comparison on success; false and out_error
    // otherwise.
    static bool ComparePeriods(const std::string& period_a, const std::string& period_b,
                                const std::string& category,
                                FinancePeriodComparison& out_comparison, std::string& out_error);

    // Creates a new funding goal named `name` with the given target amount and optional
    // "YYYY-MM-DD" deadline (empty for none). Returns true and fills out_goal (in particular its
    // new `id`, e.g. "F1") on success; false and out_error otherwise (e.g. a goal with that name
    // already exists).
    static bool AddFundingGoal(const std::string& name, long long target_amount,
                                const std::string& deadline, FinanceFundingGoal& out_goal,
                                std::string& out_error);

    // Fetches one funding goal's progress by name. Returns true and fills out_goal on success;
    // false and out_error otherwise (e.g. no such goal).
    static bool GetFundingGoal(const std::string& name, FinanceFundingGoal& out_goal,
                                std::string& out_error);

    // Fetches every funding goal's progress. Returns true and fills out_list on success (empty
    // if there are no goals); false and out_error otherwise.
    static bool ListFundingGoals(FinanceFundingGoalList& out_list, std::string& out_error);

    // Deletes the funding goal named `name`. Returns true on success; false and out_error
    // otherwise (e.g. no such goal).
    static bool DeleteFundingGoal(const std::string& name, std::string& out_error);
};

#endif  // FINANCE_SERVICE_H
