#include "finance_mcp_tool.h"

#include <cJSON.h>
#include <esp_log.h>

#include "finance_service.h"

#define TAG "FinanceMcpTool"

namespace {

constexpr const char* kCategoryHelp =
    "Category for the transaction, matching one of the 14 standardized categories in column F: "
    "expense categories `housing`, `travel`, `subscriptions`, `transport`, `personal`, "
    "`utilities`, `food`, `shopping`, `family`, `health`; income categories `salary`, `investment`; "
    "debt category `debt` (negative means paying debt down, positive means borrowing); or `other` "
    "for anything else. Map the user's spoken category (in any language) to the closest one, e.g. "
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
            "for `debt`, whose sign is trusted exactly as given and never "
            "auto-corrected: negative means paying debt down, positive means borrowing or "
            "reversing a debt payment. Get the sign right yourself for `debt` -- e.g. 'tra no 2 trieu' "
            "(pay debt 2M) -> category `debt`, amount -2000000; 'vay 5 trieu' (borrow 5M) -> "
            "category `debt`, amount +5000000.\n"
            "  `category`: ") +
            kCategoryHelp +
            "\n"
            "  `description`: Optional short free-text note, e.g. the original phrase.\n"
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
        "  `amount`: The corrected amount in VND. If `category` is `debt`, its sign is "
        "trusted as given -- see self.finance.add_transaction for what each sign means.\n"
        "  `category`: The corrected category -- see self.finance.add_transaction for the list.\n"
        "  `description`: The corrected (or unchanged) description.\n"
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
        "self.finance.get_monthly_spending",
        "Get current month's financial metrics and category spending directly from the spreadsheet (columns F and H).\n"
        "Use this tool when the user asks about this month's finances:\n"
        "- 'tháng này tôi đã tiêu bao nhiêu tiền' (how much did I spend this month): report `spent` (income H2 minus remaining H3, i.e. H2 - H3).\n"
        "- 'tháng này tôi còn bao nhiêu tiền' (how much money do I have left / remaining balance this month): report `remaining` (from column H3).\n"
        "- 'tháng này tiền ăn / tiền xăng / [category] hết bao nhiêu' (how much spent on a specific category this month): pass `category` (e.g. `food`, `transport`, `housing`), report `category_amount`.\n"
        "- 'chi tiêu theo danh mục tháng này' or 'tháng này tiêu những gì' (breakdown of expenses): report the `categories` list.\n"
        "Args:\n"
        "  `category`: (Optional) Standard category name to check specifically (e.g. `food`, `transport`, `shopping`, `housing`, `utilities`, `health`, `subscriptions`, `personal`, `family`, `debt`, `other`). Leave empty if asking for general spent/remaining or full breakdown.\n"
        "Return:\n"
        "  A JSON object with `spent` (VND, H2 - H3), `remaining` (VND, H3), `income` (VND, H2), and `categories` list.",
        PropertyList({
            Property("category", kPropertyTypeString, std::string("")),
        }),
        [](const PropertyList& properties) -> ToolResult {
            return HandleGetMonthlySpending(properties);
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


ToolResult FinanceMcpTool::HandleGetMonthlySpending(const PropertyList& properties) {
    auto category = properties["category"].value<std::string>();

    FinanceMonthlySpending spending;
    std::string error;
    if (!FinanceService::GetMonthlySpending(category, spending, error)) {
        return std::unexpected(error);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddNumberToObject(root, "spent", static_cast<double>(spending.spent));
    cJSON_AddNumberToObject(root, "remaining", static_cast<double>(spending.remaining));
    cJSON_AddNumberToObject(root, "income", static_cast<double>(spending.income));

    if (!spending.category.empty()) {
        cJSON_AddStringToObject(root, "category", spending.category.c_str());
        cJSON_AddBoolToObject(root, "category_found", spending.category_found);
        cJSON_AddNumberToObject(root, "category_amount", static_cast<double>(spending.category_amount));
    }

    cJSON* categories = cJSON_CreateArray();
    if (categories != nullptr) {
        cJSON_AddItemToObject(root, "categories", categories);
        for (const auto& item : spending.categories) {
            cJSON* cat_obj = cJSON_CreateObject();
            if (cat_obj != nullptr) {
                cJSON_AddStringToObject(cat_obj, "category", item.category.c_str());
                cJSON_AddNumberToObject(cat_obj, "amount", static_cast<double>(item.amount));
                cJSON_AddItemToArray(categories, cat_obj);
            }
        }
    }

    return root;
}



