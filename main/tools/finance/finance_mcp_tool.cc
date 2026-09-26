#include "finance_mcp_tool.h"

#include <cJSON.h>
#include <esp_log.h>

#include "finance_service.h"

#define TAG "FinanceMcpTool"

namespace {

constexpr const char* kCategoryHelp =
    "Exactly one of: expense categories `food`, `transport`, `shopping`, `housing`, "
    "`utilities`, `health`, `entertainment`, `education`, `travel`, `subscriptions`, `family`, "
    "`personal`; income categories `salary`, `bonus`, `freelance`, `investment`, `gift`, "
    "`refund`; funding categories `savings`, `debt` (unlike every category above, these go "
    "either direction -- see the `amount` arg below for how to set their sign); or `other` for "
    "anything else. Map the user's spoken category (in any language) to the closest one, e.g. "
    "'an uong'/'com nuoc' -> `food`, 'di lai'/'xang xe' -> `transport`, 'hoa don dien nuoc' -> "
    "`utilities`, 'thue nha' -> `housing`, 'luong' -> `salary`, 'tra no'/'no' -> `debt`. Defaults "
    "to `other` if nothing fits.";

constexpr const char* kPeriodHelp =
    "One of `today`, `yesterday`, `week` (Monday-Sunday containing today), `month` (the "
    "default; a billing-style cycle from the 25th of the previous calendar month through the "
    "24th of the named one, not the calendar month -- e.g. cycle `2026-09` runs 2026-08-25 to "
    "2026-09-24), an explicit cycle `YYYY-MM` (same rule), or an explicit day `YYYY-MM-DD`.";

}  // namespace

void FinanceMcpTool::Initialize() {
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddTool(
        "self.finance.add_transaction",
        std::string(
            "Record one expense or income transaction in the user's personal finance ledger. "
            "Use this when the user mentions spending or receiving money, e.g. 'an trua het 80 "
            "nghin' (lunch, 80 thousand dong) or 'vua nhan luong 15 trieu' (just got paid 15 "
            "million). Convert the spoken amount to a plain VND integer yourself before calling "
            "(80 nghin -> 80000, 1 trieu 2 -> 1200000).\n"
            "Args:\n"
            "  `amount`: Amount in VND. Positive for income, negative for expense (like a bank "
            "statement) -- this may be corrected automatically to match the category, EXCEPT "
            "for `savings`/`debt`, whose sign is trusted exactly as given and never "
            "auto-corrected: negative means money going into savings or paying debt down (goal "
            "progress increases), positive means withdrawing savings back out or reversing a "
            "debt payment (goal progress decreases). Get the sign right yourself for those two "
            "categories -- e.g. 'rut 500 nghin tu tiet kiem mua xe may' (withdraw 500k from the "
            "motorbike savings) -> category `savings`, amount +500000.\n"
            "  `category`: ") +
            kCategoryHelp +
            "\n"
            "  `description`: Optional short free-text note, e.g. the original phrase. If "
            "`category` is `savings` or `debt` and this money is progress towards an existing "
            "funding goal, call self.finance.list_funding_goals first and append that goal's "
            "`id` (e.g. \"F1\") to the end of this description -- keep the rest of the "
            "description as is, don't replace it -- so the ledger entry can be automatically "
            "linked back to that goal's tracked progress. Use the `id`, not the goal's name.\n"
            "  `date`: `today` (default), `yesterday`, `day_before_yesterday`, or an explicit "
            "`YYYY-MM-DD` -- use this for phrases like 'hom qua' (yesterday) or 'hom kia' (day "
            "before yesterday). Leave as `today` if no date was mentioned.\n"
            "Return:\n"
            "  A JSON object confirming the transaction was recorded, including its `id` -- "
            "keep this in mind in case the user immediately asks to undo/delete/correct it.",
        PropertyList({
            Property("amount", kPropertyTypeInteger, -100000000, 100000000),
            Property("category", kPropertyTypeString, std::string("other")),
            Property("description", kPropertyTypeString, std::string("")),
            Property("date", kPropertyTypeString, std::string("today")),
        }),
        [](const PropertyList& properties) -> ToolResult {
            return HandleAddTransaction(properties);
        });

    mcp_server.AddTool(
        "self.finance.update_transaction",
        "Correct a previously recorded transaction's amount, category, and/or description. Use "
        "this when the user says an earlier entry was wrong, e.g. 'khoan an trua 80 nghin luc "
        "nay thuc ra la 100 nghin' (that 80k lunch was actually 100k). Find the transaction "
        "first with self.finance.list_transactions if you don't already have its id from this "
        "conversation; if more than one recent transaction could match, ask the user which one "
        "before calling. Always pass the full corrected amount/category/description (not just "
        "the field that changed) -- reuse the existing values for fields the user didn't ask to "
        "change.\n"
        "Args:\n"
        "  `id`: The transaction id to update.\n"
        "  `amount`: The corrected amount in VND. If `category` is `savings`/`debt`, its sign is "
        "trusted as given -- see self.finance.add_transaction for what each sign means.\n"
        "  `category`: The corrected category -- see self.finance.add_transaction for the list.\n"
        "  `description`: The corrected (or unchanged) description. If `category` is `savings` "
        "or `debt` and it's not already there, append the exact funding goal id to the end -- "
        "see self.finance.add_transaction.\n"
        "Return:\n"
        "  A JSON object confirming the update.",
        PropertyList({
            Property("id", kPropertyTypeString),
            Property("amount", kPropertyTypeInteger, -100000000, 100000000),
            Property("category", kPropertyTypeString, std::string("other")),
            Property("description", kPropertyTypeString, std::string("")),
        }),
        [](const PropertyList& properties) -> ToolResult {
            return HandleUpdateTransaction(properties);
        });

    mcp_server.AddTool(
        "self.finance.get_summary",
        std::string(
            "Get income, expense, and net totals (plus transaction count) for a period, from "
            "transactions previously recorded via self.finance.add_transaction. Use this when "
            "the user asks how much they've spent or earned, e.g. 'hom nay toi tieu bao nhieu' "
            "(how much did I spend today), 'tuan nay' (this week), 'thang nay' (this month), "
            "'tinh hinh tai chinh thang nay the nao' (how's my finances this month).\n"
            "Args:\n"
            "  `period`: ") +
            kPeriodHelp +
            "\n"
            "Return:\n"
            "  A JSON object with `period`, `income` (VND), `expense` (VND), `net` (VND, "
            "income - expense), and `count` (number of transactions).",
        PropertyList({
            Property("period", kPropertyTypeString, std::string("month")),
        }),
        [](const PropertyList& properties) -> ToolResult {
            return HandleGetSummary(properties);
        });

    mcp_server.AddTool(
        "self.finance.get_category_summary",
        std::string(
            "Get a breakdown by category for a period, sorted by amount, from transactions "
            "previously recorded via self.finance.add_transaction. Use this when the user asks "
            "what they spent money on, e.g. 'thang nay toi tieu vao nhung khoan nao' (what did "
            "I spend on this month) or 'thang nay toi tieu bao nhieu tien an' (how much did I "
            "spend on food this month -- look up the `food` entry in the result).\n"
            "Args:\n"
            "  `period`: ") +
            kPeriodHelp +
            "\n"
            "Return:\n"
            "  A JSON object with `period` and `categories`, a list of `{category, total, "
            "count}` sorted by |total| descending. `total` is positive for income categories, "
            "negative for expense categories.",
        PropertyList({
            Property("period", kPropertyTypeString, std::string("month")),
        }),
        [](const PropertyList& properties) -> ToolResult {
            return HandleGetCategorySummary(properties);
        });

    mcp_server.AddTool(
        "self.finance.list_transactions",
        std::string(
            "List individual transactions previously recorded via self.finance.add_transaction, "
            "most recent first. Use this when the user asks to see/review transactions, e.g. "
            "'liet ke cac khoan toi da chi hom nay' (list what I spent today), or to look up a "
            "transaction's id before calling self.finance.update_transaction or "
            "self.finance.delete_transaction. Summarize the result in speech instead of reading "
            "every field, unless the user asks for detail.\n"
            "Args:\n"
            "  `period`: ") +
            kPeriodHelp +
            " Leave empty to list across all time.\n"
            "  `limit`: Maximum number of transactions to return (1-15). Defaults to 5.\n"
            "Return:\n"
            "  A JSON object with `period`, `count`, and `transactions`, a list of `{id, date, "
            "description, amount, category}`.",
        PropertyList({
            Property("period", kPropertyTypeString, std::string("")),
            Property("limit", kPropertyTypeInteger, 5, 1, 15),
        }),
        [](const PropertyList& properties) -> ToolResult {
            return HandleListTransactions(properties);
        });

    mcp_server.AddTool(
        "self.finance.delete_transaction",
        "Delete one previously recorded transaction by its id. Use this when the user asks to "
        "remove or undo a transaction, e.g. 'xoa giao dich vua roi' (delete the last "
        "transaction) or 'xoa khoan Grab 50 nghin vua roi' (delete that 50k Grab charge) -- get "
        "the id from the most recent self.finance.add_transaction result, or from "
        "self.finance.list_transactions if it's an older entry. If more than one transaction "
        "could match what the user described, ask which one before deleting.\n"
        "Args:\n"
        "  `id`: The transaction id to delete.\n"
        "Return:\n"
        "  A JSON object confirming the deletion.",
        PropertyList({
            Property("id", kPropertyTypeString),
        }),
        [](const PropertyList& properties) -> ToolResult {
            return HandleDeleteTransaction(properties);
        });

    mcp_server.AddTool(
        "self.finance.set_budget",
        "Set (or replace) a category's recurring monthly budget limit. Use this when the user "
        "sets a spending target, e.g. 'dat ngan sach an uong thang nay la 5 trieu' (set this "
        "month's food budget to 5 million) -- the budget recurs every month until changed.\n"
        "Args:\n"
        "  `category`: The category to budget -- see self.finance.add_transaction for the list.\n"
        "  `limit`: The monthly budget limit in VND.\n"
        "Return:\n"
        "  A JSON object confirming the budget was set.",
        PropertyList({
            Property("category", kPropertyTypeString, std::string("other")),
            Property("limit", kPropertyTypeInteger, 0, 2000000000),
        }),
        [](const PropertyList& properties) -> ToolResult { return HandleSetBudget(properties); });

    mcp_server.AddTool(
        "self.finance.get_budget",
        std::string(
            "Get a category's budget status for a period: how much of its budget (set via "
            "self.finance.set_budget) has been spent. Use this when the user asks about budget "
            "usage, e.g. 'toi da dung bao nhieu ngan sach an uong' (how much of my food budget "
            "have I used). `status` is `warning` at 80% spent and `exceeded` at 100%+ -- mention "
            "this to the user when it's not `ok`.\n"
            "Args:\n"
            "  `category`: The category to check -- see self.finance.add_transaction for the "
            "list.\n"
            "  `period`: ") +
            kPeriodHelp +
            " Defaults to `month`, matching the budget's recurring cycle.\n"
            "Return:\n"
            "  A JSON object with `category`, `budget`, `spent`, `remaining` (all VND), "
            "`percentage`, and `status` (`ok`/`warning`/`exceeded`). Fails if no budget is set "
            "for that category.",
        PropertyList({
            Property("category", kPropertyTypeString, std::string("other")),
            Property("period", kPropertyTypeString, std::string("month")),
        }),
        [](const PropertyList& properties) -> ToolResult { return HandleGetBudget(properties); });

    mcp_server.AddTool(
        "self.finance.compare_periods",
        std::string(
            "Compare spending between two periods, either overall or for one category. Use this "
            "when the user asks to compare, e.g. 'thang nay toi tieu nhieu hon thang truoc bao "
            "nhieu' (how much more did I spend this month than last month) or 'so sanh tien an "
            "thang nay va thang truoc' (compare food spending this month vs last month).\n"
            "Args:\n"
            "  `period_a`: ") +
            kPeriodHelp +
            " Defaults to `month`.\n"
            "  `period_b`: Same format as `period_a`. Leave empty to compare against the period "
            "immediately before `period_a` (e.g. the previous calendar month).\n"
            "  `category`: Leave empty to compare overall expense totals; or one category (see "
            "self.finance.add_transaction for the list) to compare just that category.\n"
            "Return:\n"
            "  A JSON object with `period_a`, `period_b`, `value_a`, `value_b` (VND), "
            "`difference` (value_a - value_b), and `percent_change`.",
        PropertyList({
            Property("period_a", kPropertyTypeString, std::string("month")),
            Property("period_b", kPropertyTypeString, std::string("")),
            Property("category", kPropertyTypeString, std::string("")),
        }),
        [](const PropertyList& properties) -> ToolResult {
            return HandleComparePeriods(properties);
        });

    mcp_server.AddTool(
        "self.finance.add_funding_goal",
        "Create a new named funding goal with a target amount -- either a savings target (money "
        "to accumulate towards a purchase, logged via the `savings` transaction category) or a "
        "debt target (money owed that's being paid off, logged via `debt`). Use this when the "
        "user wants to start saving towards something or start tracking a debt, e.g. 'tao muc "
        "tieu tiet kiem mua xe may 20 trieu' (create a funding goal to buy a motorbike, 20 "
        "million) or 'de danh mua laptop moi, han cuoi nam nay' (save up for a new laptop, "
        "deadline end of this year) or 'tao khoan tra no the tin dung 10 trieu' (create a "
        "funding goal to pay off 10 million of credit card debt). Convert the spoken amount to "
        "a plain VND integer yourself (20 trieu -> 20000000).\n"
        "Args:\n"
        "  `name`: Short name for the goal, e.g. the original phrase like 'mua xe may'. Must be "
        "unique -- fails if a goal with this exact name already exists.\n"
        "  `target_amount`: The savings or debt target in VND.\n"
        "  `deadline`: Optional explicit `YYYY-MM-DD` the user wants to hit the target by. Leave "
        "empty if no deadline was mentioned.\n"
        "Return:\n"
        "  A JSON object confirming the goal was created, including its `id` (e.g. \"F1\") -- "
        "keep this in mind, since self.finance.add_transaction needs it (not the name) to link a "
        "transaction back to this goal.",
        PropertyList({
            Property("name", kPropertyTypeString),
            Property("target_amount", kPropertyTypeInteger, 0, 2000000000),
            Property("deadline", kPropertyTypeString, std::string("")),
        }),
        [](const PropertyList& properties) -> ToolResult {
            return HandleAddFundingGoal(properties);
        });

    mcp_server.AddTool(
        "self.finance.get_funding_goal",
        "Get one funding goal's progress. Use this when the user asks how a specific savings or "
        "debt goal is doing, e.g. 'toi tiet kiem duoc bao nhieu cho muc tieu mua xe may roi' "
        "(how much have I saved for the motorbike goal).\n"
        "Args:\n"
        "  `name`: The goal's exact name, as given to self.finance.add_funding_goal.\n"
        "Return:\n"
        "  A JSON object with `id` (e.g. \"F1\" -- what self.finance.add_transaction needs to "
        "link a transaction to this goal), `target`, `saved`, `remaining` (all VND), "
        "`percentage`, `completed`, `deadline` (empty if none), and `completed_date` (empty if "
        "not filled in yet). Fails if no goal exists with that name.",
        PropertyList({
            Property("name", kPropertyTypeString),
        }),
        [](const PropertyList& properties) -> ToolResult {
            return HandleGetFundingGoal(properties);
        });

    mcp_server.AddTool(
        "self.finance.list_funding_goals",
        "List every funding goal and its progress. Use this when the user asks to see all their "
        "savings/debt goals, e.g. 'toi dang co nhung muc tieu tiet kiem nao' (what savings goals "
        "do I have). Summarize in speech instead of reading every field, unless asked for "
        "detail.\n"
        "Return:\n"
        "  A JSON object with `goals`, a list of `{id, name, target, saved, remaining, "
        "percentage, completed, deadline, completed_date}`.",
        PropertyList(),
        [](const PropertyList& properties) -> ToolResult {
            return HandleListFundingGoals(properties);
        });

    mcp_server.AddTool(
        "self.finance.delete_funding_goal",
        "Delete a funding goal by name. Use this when the user asks to remove a goal, e.g. 'xoa "
        "muc tieu tiet kiem mua xe may' (delete the motorbike funding goal).\n"
        "Args:\n"
        "  `name`: The goal's exact name, as given to self.finance.add_funding_goal.\n"
        "Return:\n"
        "  A JSON object confirming the deletion.",
        PropertyList({
            Property("name", kPropertyTypeString),
        }),
        [](const PropertyList& properties) -> ToolResult {
            return HandleDeleteFundingGoal(properties);
        });

    ESP_LOGI(TAG, "FinanceMcpTool initialized");
}

ToolResult FinanceMcpTool::HandleAddTransaction(const PropertyList& properties) {
    auto amount = properties["amount"].value<int>();
    auto category = properties["category"].value<std::string>();
    auto description = properties["description"].value<std::string>();
    auto date = properties["date"].value<std::string>();

    FinanceAddResult result;
    std::string error;
    if (!FinanceService::AddTransaction(amount, category, description, date, result, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddBoolToObject(root, "recorded", true);
    cJSON_AddStringToObject(root, "id", result.id.c_str());
    cJSON_AddNumberToObject(root, "amount", result.amount);
    cJSON_AddStringToObject(root, "category", result.category.c_str());
    return root;
}

ToolResult FinanceMcpTool::HandleUpdateTransaction(const PropertyList& properties) {
    auto id = properties["id"].value<std::string>();
    auto amount = properties["amount"].value<int>();
    auto category = properties["category"].value<std::string>();
    auto description = properties["description"].value<std::string>();

    std::string error;
    if (!FinanceService::UpdateTransaction(id, amount, category, description, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddBoolToObject(root, "updated", true);
    cJSON_AddStringToObject(root, "id", id.c_str());
    cJSON_AddNumberToObject(root, "amount", amount);
    cJSON_AddStringToObject(root, "category", category.c_str());
    return root;
}

ToolResult FinanceMcpTool::HandleGetSummary(const PropertyList& properties) {
    auto period = properties["period"].value<std::string>();

    FinanceSummary summary;
    std::string error;
    if (!FinanceService::GetSummary(period, summary, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddStringToObject(root, "period", summary.period.c_str());
    cJSON_AddNumberToObject(root, "income", static_cast<double>(summary.income));
    cJSON_AddNumberToObject(root, "expense", static_cast<double>(summary.expense));
    cJSON_AddNumberToObject(root, "net", static_cast<double>(summary.net));
    cJSON_AddNumberToObject(root, "count", summary.count);
    return root;
}

ToolResult FinanceMcpTool::HandleGetCategorySummary(const PropertyList& properties) {
    auto period = properties["period"].value<std::string>();

    FinanceCategorySummary summary;
    std::string error;
    if (!FinanceService::GetCategorySummary(period, summary, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddStringToObject(root, "period", summary.period.c_str());

    cJSON* categories = cJSON_CreateArray();
    if (categories == nullptr) {
        cJSON_Delete(root);
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddItemToObject(root, "categories", categories);

    for (const auto& category : summary.categories) {
        cJSON* item = cJSON_CreateObject();
        if (item == nullptr) continue;
        cJSON_AddStringToObject(item, "category", category.category.c_str());
        cJSON_AddNumberToObject(item, "total", static_cast<double>(category.total));
        cJSON_AddNumberToObject(item, "count", category.count);
        cJSON_AddItemToArray(categories, item);
    }

    return root;
}

ToolResult FinanceMcpTool::HandleListTransactions(const PropertyList& properties) {
    auto period = properties["period"].value<std::string>();
    auto limit = properties["limit"].value<int>();

    FinanceTransactionList list;
    std::string error;
    if (!FinanceService::ListTransactions(period, limit, list, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddStringToObject(root, "period", list.period.empty() ? "all" : list.period.c_str());
    cJSON_AddNumberToObject(root, "count", static_cast<double>(list.transactions.size()));

    cJSON* transactions = cJSON_CreateArray();
    if (transactions == nullptr) {
        cJSON_Delete(root);
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddItemToObject(root, "transactions", transactions);

    for (const auto& transaction : list.transactions) {
        cJSON* item = cJSON_CreateObject();
        if (item == nullptr) continue;
        cJSON_AddStringToObject(item, "id", transaction.id.c_str());
        cJSON_AddStringToObject(item, "date", transaction.date.c_str());
        cJSON_AddStringToObject(item, "description", transaction.description.c_str());
        cJSON_AddNumberToObject(item, "amount", static_cast<double>(transaction.amount));
        cJSON_AddStringToObject(item, "category", transaction.category.c_str());
        cJSON_AddItemToArray(transactions, item);
    }

    return root;
}

ToolResult FinanceMcpTool::HandleDeleteTransaction(const PropertyList& properties) {
    auto id = properties["id"].value<std::string>();

    std::string error;
    if (!FinanceService::DeleteTransaction(id, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddBoolToObject(root, "deleted", true);
    cJSON_AddStringToObject(root, "id", id.c_str());
    return root;
}

ToolResult FinanceMcpTool::HandleSetBudget(const PropertyList& properties) {
    auto category = properties["category"].value<std::string>();
    auto limit = properties["limit"].value<int>();

    std::string error;
    if (!FinanceService::SetBudget(category, limit, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddBoolToObject(root, "set", true);
    cJSON_AddStringToObject(root, "category", category.c_str());
    cJSON_AddNumberToObject(root, "limit", limit);
    return root;
}

ToolResult FinanceMcpTool::HandleGetBudget(const PropertyList& properties) {
    auto category = properties["category"].value<std::string>();
    auto period = properties["period"].value<std::string>();

    FinanceBudgetStatus status;
    std::string error;
    if (!FinanceService::GetBudgetStatus(category, period, status, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddStringToObject(root, "category", status.category.c_str());
    cJSON_AddNumberToObject(root, "budget", static_cast<double>(status.budget));
    cJSON_AddNumberToObject(root, "spent", static_cast<double>(status.spent));
    cJSON_AddNumberToObject(root, "remaining", static_cast<double>(status.remaining));
    cJSON_AddNumberToObject(root, "percentage", status.percentage);
    cJSON_AddStringToObject(root, "status", status.status.c_str());
    return root;
}

ToolResult FinanceMcpTool::HandleComparePeriods(const PropertyList& properties) {
    auto period_a = properties["period_a"].value<std::string>();
    auto period_b = properties["period_b"].value<std::string>();
    auto category = properties["category"].value<std::string>();

    FinancePeriodComparison comparison;
    std::string error;
    if (!FinanceService::ComparePeriods(period_a, period_b, category, comparison, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddStringToObject(root, "period_a", comparison.period_a.c_str());
    cJSON_AddStringToObject(root, "period_b", comparison.period_b.c_str());
    if (!comparison.category.empty()) {
        cJSON_AddStringToObject(root, "category", comparison.category.c_str());
    }
    cJSON_AddNumberToObject(root, "value_a", static_cast<double>(comparison.value_a));
    cJSON_AddNumberToObject(root, "value_b", static_cast<double>(comparison.value_b));
    cJSON_AddNumberToObject(root, "difference", static_cast<double>(comparison.difference));
    cJSON_AddNumberToObject(root, "percent_change", comparison.percent_change);
    return root;
}

namespace {

cJSON* FundingGoalToJson(const FinanceFundingGoal& goal) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) return nullptr;
    cJSON_AddStringToObject(root, "id", goal.id.c_str());
    cJSON_AddStringToObject(root, "name", goal.name.c_str());
    cJSON_AddNumberToObject(root, "target", static_cast<double>(goal.target));
    cJSON_AddNumberToObject(root, "saved", static_cast<double>(goal.saved));
    cJSON_AddNumberToObject(root, "remaining", static_cast<double>(goal.remaining));
    cJSON_AddNumberToObject(root, "percentage", goal.percentage);
    cJSON_AddBoolToObject(root, "completed", goal.completed);
    cJSON_AddStringToObject(root, "deadline", goal.deadline.c_str());
    cJSON_AddStringToObject(root, "completed_date", goal.completed_date.c_str());
    return root;
}

}  // namespace

ToolResult FinanceMcpTool::HandleAddFundingGoal(const PropertyList& properties) {
    auto name = properties["name"].value<std::string>();
    auto target_amount = properties["target_amount"].value<int>();
    auto deadline = properties["deadline"].value<std::string>();

    FinanceFundingGoal goal;
    std::string error;
    if (!FinanceService::AddFundingGoal(name, target_amount, deadline, goal, error)) {
        return std::unexpected(error);
    }

    cJSON* root = FundingGoalToJson(goal);
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddBoolToObject(root, "created", true);
    return root;
}

ToolResult FinanceMcpTool::HandleGetFundingGoal(const PropertyList& properties) {
    auto name = properties["name"].value<std::string>();

    FinanceFundingGoal goal;
    std::string error;
    if (!FinanceService::GetFundingGoal(name, goal, error)) {
        return std::unexpected(error);
    }

    cJSON* root = FundingGoalToJson(goal);
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    return root;
}

ToolResult FinanceMcpTool::HandleListFundingGoals(const PropertyList& properties) {
    FinanceFundingGoalList list;
    std::string error;
    if (!FinanceService::ListFundingGoals(list, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }

    cJSON* goals = cJSON_CreateArray();
    if (goals == nullptr) {
        cJSON_Delete(root);
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddItemToObject(root, "goals", goals);

    for (const auto& goal : list.goals) {
        cJSON* item = FundingGoalToJson(goal);
        if (item == nullptr) continue;
        cJSON_AddItemToArray(goals, item);
    }

    return root;
}

ToolResult FinanceMcpTool::HandleDeleteFundingGoal(const PropertyList& properties) {
    auto name = properties["name"].value<std::string>();

    std::string error;
    if (!FinanceService::DeleteFundingGoal(name, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddBoolToObject(root, "deleted", true);
    cJSON_AddStringToObject(root, "name", name.c_str());
    return root;
}
