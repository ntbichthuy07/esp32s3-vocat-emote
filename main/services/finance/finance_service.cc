#include "finance_service.h"

#include <cJSON.h>
#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <memory>

#include <http.h>
#include <network_interface.h>

#include "board.h"
#include "cjson_utils.h"

#include "../service_config.h"

#define TAG "FinanceService"

namespace {

constexpr int kHttpTimeoutMs = 15000;
constexpr size_t kMaxBodyBytes = 4 * 1024;
constexpr size_t kReadChunkBytes = 512;
constexpr int kMaxRedirects = 3;

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

// Google Apps Script Web Apps answer both GET and POST requests to their /exec URL with a 302
// redirect to a one-off script.googleusercontent.com URL that serves the already-computed
// response; the redirect target must be fetched with GET regardless of the original method.
// esp-ml307's HttpClient (see managed_components/78__esp-ml307/src/http_client.cc) is a raw
// HTTP/1.1 client with no built-in redirect support, so this follows it by hand.
bool OpenWithRedirects(const std::string& method, const std::string& url, const std::string& body,
                       std::unique_ptr<Http>& out_http, std::string& out_error) {
    std::string next_method = method;
    std::string next_url = url;
    bool send_body = true;

    for (int attempt = 0; attempt <= kMaxRedirects; attempt++) {
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        if (!http) {
            out_error = "Failed to create HTTP connection";
            return false;
        }
        http->SetTimeout(kHttpTimeoutMs);
        if (send_body && !body.empty()) {
            http->SetHeader("Content-Type", "application/json");
            http->SetContent(std::string(body));
        }

        auto opened = http->Open(next_method, next_url);
        if (!opened) {
            out_error = "Failed to connect to finance sheet: " + opened.error().ToString();
            return false;
        }

        auto status_code = http->GetStatusCode();
        if (!status_code) {
            out_error = "Failed to read finance sheet response status";
            return false;
        }

        if (*status_code >= 300 && *status_code < 400) {
            std::string location = http->GetResponseHeader("location");
            http->Close();
            if (location.empty()) {
                out_error = "Finance sheet redirected without a Location header";
                return false;
            }
            next_method = "GET";
            next_url = location;
            send_body = false;
            continue;
        }

        out_http = std::move(http);
        return true;
    }

    out_error = "Too many redirects from finance sheet";
    return false;
}

// Posts a JSON request to the Apps Script Web App and returns its parsed JSON response.
// The script always answers with {"ok": true, ...} or {"ok": false, "error": "..."}.
bool CallAppsScript(cJSON* request, CJsonUniquePtr& out_response, std::string& out_error) {
    CJsonStringUniquePtr request_str(cJSON_PrintUnformatted(request));
    if (!request_str) {
        out_error = "Failed to encode finance request";
        return false;
    }

    std::unique_ptr<Http> http;
    if (!OpenWithRedirects("POST", kFinanceApiUrl, request_str.get(), http, out_error)) {
        return false;
    }

    auto status_code = http->GetStatusCode();
    std::string body = ReadBodyCapped(http.get());
    http->Close();

    if (!status_code || *status_code != 200) {
        out_error = "Finance sheet request failed with status " +
                    (status_code ? std::to_string(*status_code) : std::string("?")) +
                    (body.empty() ? "" : (": " + body));
        return false;
    }
    if (body.empty()) {
        out_error = "Finance sheet returned an empty response";
        return false;
    }

    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    if (!root) {
        out_error = "Failed to parse finance sheet response";
        return false;
    }

    auto ok = cJSON_GetObjectItem(root.get(), "ok");
    if (!cJSON_IsTrue(ok)) {
        auto error_item = cJSON_GetObjectItem(root.get(), "error");
        out_error = cJSON_IsString(error_item) ? error_item->valuestring
                                                : "Finance sheet reported failure";
        return false;
    }

    out_response = std::move(root);
    return true;
}

// Returns false if the device's clock hasn't been synced yet (see ota.cc's server_time
// handshake, which is what actually sets the wall clock here -- there's no SNTP client in this
// firmware). Mirrors the tm_year sanity check lvgl_display.cc uses before trusting the clock.
bool GetLocalNow(struct tm& out_tm) {
    time_t now = time(nullptr);
    localtime_r(&now, &out_tm);
    return out_tm.tm_year >= 2025 - 1900;
}

std::string FormatDate(const struct tm& t) {
    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
    return buf;
}

// Adds (or subtracts) whole days, letting mktime() normalize month/year rollover -- the standard
// portable idiom for date arithmetic against struct tm.
struct tm AddDays(struct tm t, int days) {
    t.tm_mday += days;
    t.tm_isdst = -1;
    mktime(&t);
    return t;
}

bool AllDigits(const std::string& s, size_t begin, size_t end) {
    for (size_t i = begin; i < end; i++) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

// "YYYY-MM-DD"
bool LooksLikeDay(const std::string& s) {
    return s.size() == 10 && s[4] == '-' && s[7] == '-' && AllDigits(s, 0, 4) &&
           AllDigits(s, 5, 7) && AllDigits(s, 8, 10);
}

// "YYYY-MM"
bool LooksLikeMonth(const std::string& s) {
    return s.size() == 7 && s[4] == '-' && AllDigits(s, 0, 4) && AllDigits(s, 5, 7);
}

// The "month" period is a billing-style cycle (25th of one calendar month through the 24th of
// the next), not the calendar month -- e.g. "thang 9" / cycle 2026-09 runs 2026-09-25 through
// 2026-10-24. A cycle is labeled by the month its 25th falls in.
constexpr int kMonthCycleStartDay = 25;

struct tm StartOfMonth(struct tm t) {
    t.tm_mday = kMonthCycleStartDay;
    t.tm_hour = t.tm_min = t.tm_sec = 0;
    t.tm_isdst = -1;
    mktime(&t);
    return t;
}

struct tm AddMonths(struct tm t, int months) {
    t.tm_mon += months;
    t.tm_isdst = -1;
    mktime(&t);
    return t;
}

// Parses "YYYY-MM-DD" into a normalized, zeroed-time struct tm (mktime fills in tm_wday etc).
struct tm ParseDate(const std::string& iso_date) {
    struct tm t = {};
    t.tm_year = std::stoi(iso_date.substr(0, 4)) - 1900;
    t.tm_mon = std::stoi(iso_date.substr(5, 2)) - 1;
    t.tm_mday = std::stoi(iso_date.substr(8, 2));
    t.tm_isdst = -1;
    mktime(&t);
    return t;
}

// Parses "YYYY-MM" -- the cycle's label, i.e. the month its 24th falls in -- into that cycle's
// start: the 25th of the *previous* month (see kMonthCycleStartDay). E.g. "2026-09" starts
// 2026-08-25.
struct tm ParseMonth(const std::string& iso_month) {
    struct tm t = {};
    t.tm_year = std::stoi(iso_month.substr(0, 4)) - 1900;
    t.tm_mon = std::stoi(iso_month.substr(5, 2)) - 1;
    t.tm_mday = kMonthCycleStartDay;
    t.tm_isdst = -1;
    mktime(&t);
    return AddMonths(t, -1);
}

// Resolves the add_transaction `date` argument ("today"/"yesterday"/"day_before_yesterday"/an
// explicit "YYYY-MM-DD"; empty defaults to "today") into a full ISO datetime. "today" uses the
// current time of day; other values use noon, since the real time isn't known for a backdated
// entry. Returns false if the device clock isn't synced or `date` isn't recognized.
bool ResolveDate(const std::string& date, std::string& out_iso_date) {
    struct tm today;
    if (!GetLocalNow(today)) return false;

    std::string effective = date.empty() ? "today" : date;
    char buf[32];

    if (effective == "today") {
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &today);
        out_iso_date = buf;
        return true;
    }

    struct tm target;
    if (effective == "yesterday") {
        target = AddDays(today, -1);
    } else if (effective == "day_before_yesterday") {
        target = AddDays(today, -2);
    } else if (LooksLikeDay(effective)) {
        target = ParseDate(effective);
    } else {
        return false;
    }

    target.tm_hour = 12;
    target.tm_min = 0;
    target.tm_sec = 0;
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &target);
    out_iso_date = buf;
    return true;
}

// Resolves a period keyword ("today", "yesterday", "week", "month"), an explicit "YYYY-MM", or
// an explicit "YYYY-MM-DD" (empty defaults to "month") into a half-open [out_start, out_end)
// date range and a human-readable label. "month" and "YYYY-MM" are a billing-style cycle (the
// 25th of one calendar month through the 24th of the next), not the calendar month -- see
// kMonthCycleStartDay. Returns false if the device clock isn't synced.
bool ResolvePeriodRange(const std::string& period, std::string& out_start, std::string& out_end,
                         std::string& out_label) {
    struct tm today;
    if (!GetLocalNow(today)) return false;

    std::string effective = period.empty() ? "month" : period;

    if (LooksLikeDay(effective)) {
        struct tm day = ParseDate(effective);
        out_start = FormatDate(day);
        out_end = FormatDate(AddDays(day, 1));
        out_label = out_start;
        return true;
    }

    if (LooksLikeMonth(effective)) {
        struct tm month_start = ParseMonth(effective);
        out_start = FormatDate(month_start);
        out_end = FormatDate(AddMonths(month_start, 1));
        out_label = effective;
        return true;
    }

    if (effective == "today") {
        out_start = FormatDate(today);
        out_end = FormatDate(AddDays(today, 1));
        out_label = "today";
        return true;
    }

    if (effective == "yesterday") {
        out_start = FormatDate(AddDays(today, -1));
        out_end = FormatDate(today);
        out_label = "yesterday";
        return true;
    }

    if (effective == "week") {
        int days_since_monday = (today.tm_wday + 6) % 7;  // tm_wday: 0=Sunday..6=Saturday
        struct tm week_start = AddDays(today, -days_since_monday);
        out_start = FormatDate(week_start);
        out_end = FormatDate(AddDays(week_start, 7));
        out_label = "week";
        return true;
    }

    // "month", or anything unrecognized: the cycle containing today, labeled by the month its
    // 24th falls in. Before the 25th, today is in the cycle labeled this month (started last
    // month); on/after the 25th, today is in the cycle labeled next month (started this month).
    struct tm label_month = today.tm_mday >= kMonthCycleStartDay ? AddMonths(today, 1) : today;
    struct tm month_end = StartOfMonth(label_month);  // the 25th of the label month
    struct tm month_start = AddMonths(month_end, -1);
    out_start = FormatDate(month_start);
    out_end = FormatDate(month_end);
    out_label = FormatDate(month_end).substr(0, 7);
    return true;
}

// Resolves the period immediately before `period` -- the previous day/week/month cycle, matching
// `period`'s own kind -- for ComparePeriods' default `period_b`. Stepping back by the exact unit
// (rather than by current_end - current_start days) keeps "previous month" a real 25th-to-25th
// cycle even though months vary in length. Returns false if the device clock isn't synced.
bool ResolvePreviousPeriodRange(const std::string& period, std::string& out_start,
                                 std::string& out_end, std::string& out_label) {
    std::string current_start, current_end, current_label;
    if (!ResolvePeriodRange(period, current_start, current_end, current_label)) return false;

    std::string effective = period.empty() ? "month" : period;
    struct tm start_tm = ParseDate(current_start);

    if (LooksLikeMonth(effective) || effective == "month") {
        struct tm prev_start = AddMonths(start_tm, -1);
        out_start = FormatDate(prev_start);
        out_end = current_start;
        // Label = the month the cycle ends in, i.e. one month after its start (see ParseMonth).
        out_label = FormatDate(AddMonths(prev_start, 1)).substr(0, 7);
        return true;
    }

    if (effective == "week") {
        out_start = FormatDate(AddDays(start_tm, -7));
        out_end = current_start;
        out_label = "previous week";
        return true;
    }

    // "today", "yesterday", or an explicit day: the single day before it.
    out_start = FormatDate(AddDays(start_tm, -1));
    out_end = current_start;
    out_label = out_start;
    return true;
}

// Shared by FinanceService::GetSummary and ComparePeriods, which needs to fetch a summary for an
// already-resolved [start, end) range (e.g. "the period before period_a") rather than resolving
// a period keyword itself.
bool GetSummaryForRange(const std::string& start, const std::string& end,
                        const std::string& label, FinanceSummary& out_summary,
                        std::string& out_error) {
    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate finance request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "token", kFinanceApiSecret);
    cJSON_AddStringToObject(request.get(), "action", "get_summary");
    cJSON_AddStringToObject(request.get(), "start", start.c_str());
    cJSON_AddStringToObject(request.get(), "end", end.c_str());

    ESP_LOGI(TAG, "Getting finance summary for %s (%s..%s)", label.c_str(), start.c_str(),
             end.c_str());

    CJsonUniquePtr response;
    if (!CallAppsScript(request.get(), response, out_error)) {
        return false;
    }

    auto income = cJSON_GetObjectItem(response.get(), "income");
    auto expense = cJSON_GetObjectItem(response.get(), "expense");
    auto net = cJSON_GetObjectItem(response.get(), "net");
    auto count = cJSON_GetObjectItem(response.get(), "count");
    out_summary.period = label;
    out_summary.income = cJSON_IsNumber(income) ? static_cast<long long>(income->valuedouble) : 0;
    out_summary.expense =
        cJSON_IsNumber(expense) ? static_cast<long long>(expense->valuedouble) : 0;
    out_summary.net = cJSON_IsNumber(net) ? static_cast<long long>(net->valuedouble) : 0;
    out_summary.count = cJSON_IsNumber(count) ? count->valueint : 0;
    return true;
}

// Shared by FinanceService::GetCategorySummary and ComparePeriods -- see GetSummaryForRange.
bool GetCategorySummaryForRange(const std::string& start, const std::string& end,
                                const std::string& label, FinanceCategorySummary& out_summary,
                                std::string& out_error) {
    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate finance request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "token", kFinanceApiSecret);
    cJSON_AddStringToObject(request.get(), "action", "get_category_summary");
    cJSON_AddStringToObject(request.get(), "start", start.c_str());
    cJSON_AddStringToObject(request.get(), "end", end.c_str());

    ESP_LOGI(TAG, "Getting finance category summary for %s (%s..%s)", label.c_str(),
             start.c_str(), end.c_str());

    CJsonUniquePtr response;
    if (!CallAppsScript(request.get(), response, out_error)) {
        return false;
    }

    out_summary.period = label;
    out_summary.categories.clear();

    auto categories = cJSON_GetObjectItem(response.get(), "categories");
    if (cJSON_IsArray(categories)) {
        cJSON* entry;
        cJSON_ArrayForEach(entry, categories) {
            auto category = cJSON_GetObjectItem(entry, "category");
            auto total = cJSON_GetObjectItem(entry, "total");
            auto count = cJSON_GetObjectItem(entry, "count");

            FinanceCategoryTotal item;
            item.category = cJSON_IsString(category) ? category->valuestring : "";
            item.total = cJSON_IsNumber(total) ? static_cast<long long>(total->valuedouble) : 0;
            item.count = cJSON_IsNumber(count) ? count->valueint : 0;
            out_summary.categories.push_back(std::move(item));
        }
    }
    return true;
}

}  // namespace

bool FinanceService::AddTransaction(int amount, const std::string& category,
                                     const std::string& description, const std::string& date,
                                     FinanceAddResult& out_result, std::string& out_error) {
    std::string iso_date;
    if (!ResolveDate(date, iso_date)) {
        out_error = date.empty() || date == "today"
                        ? "Device clock is not synced yet; try again in a moment"
                        : "Unrecognized date: " + date;
        return false;
    }

    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate finance request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "token", kFinanceApiSecret);
    cJSON_AddStringToObject(request.get(), "action", "add_transaction");
    cJSON_AddStringToObject(request.get(), "date", iso_date.c_str());
    cJSON_AddNumberToObject(request.get(), "amount", amount);
    cJSON_AddStringToObject(request.get(), "category", category.c_str());
    cJSON_AddStringToObject(request.get(), "note", description.c_str());

    ESP_LOGI(TAG, "Adding transaction: %d %s (%s) on %s", amount, category.c_str(),
             description.c_str(), iso_date.c_str());

    CJsonUniquePtr response;
    if (!CallAppsScript(request.get(), response, out_error)) {
        return false;
    }

    auto id = cJSON_GetObjectItem(response.get(), "id");
    auto normalized_category = cJSON_GetObjectItem(response.get(), "category");
    auto normalized_amount = cJSON_GetObjectItem(response.get(), "amount");
    out_result.id = cJSON_IsString(id) ? id->valuestring : "";
    out_result.category = cJSON_IsString(normalized_category) ? normalized_category->valuestring
                                                                : category;
    out_result.amount =
        cJSON_IsNumber(normalized_amount) ? normalized_amount->valueint : amount;
    return true;
}

bool FinanceService::UpdateTransaction(const std::string& id, int amount,
                                        const std::string& category,
                                        const std::string& description, std::string& out_error) {
    if (id.empty()) {
        out_error = "Missing transaction id";
        return false;
    }

    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate finance request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "token", kFinanceApiSecret);
    cJSON_AddStringToObject(request.get(), "action", "update_transaction");
    cJSON_AddStringToObject(request.get(), "id", id.c_str());
    cJSON_AddNumberToObject(request.get(), "amount", amount);
    cJSON_AddStringToObject(request.get(), "category", category.c_str());
    cJSON_AddStringToObject(request.get(), "note", description.c_str());

    ESP_LOGI(TAG, "Updating finance transaction %s: %d %s (%s)", id.c_str(), amount,
             category.c_str(), description.c_str());

    CJsonUniquePtr response;
    return CallAppsScript(request.get(), response, out_error);
}

bool FinanceService::DeleteTransaction(const std::string& id, std::string& out_error) {
    if (id.empty()) {
        out_error = "Missing transaction id";
        return false;
    }

    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate finance request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "token", kFinanceApiSecret);
    cJSON_AddStringToObject(request.get(), "action", "delete_transaction");
    cJSON_AddStringToObject(request.get(), "id", id.c_str());

    ESP_LOGI(TAG, "Deleting finance transaction %s", id.c_str());

    CJsonUniquePtr response;
    return CallAppsScript(request.get(), response, out_error);
}

bool FinanceService::ListTransactions(const std::string& period, int limit,
                                       FinanceTransactionList& out_list, std::string& out_error) {
    out_list.period = period;
    out_list.transactions.clear();

    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate finance request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "token", kFinanceApiSecret);
    cJSON_AddStringToObject(request.get(), "action", "list_transactions");
    cJSON_AddNumberToObject(request.get(), "limit", limit);

    if (period.empty()) {
        cJSON_AddStringToObject(request.get(), "start", "");
        cJSON_AddStringToObject(request.get(), "end", "");
    } else {
        std::string start, end, label;
        if (!ResolvePeriodRange(period, start, end, label)) {
            out_error = "Device clock is not synced yet; try again in a moment";
            return false;
        }
        cJSON_AddStringToObject(request.get(), "start", start.c_str());
        cJSON_AddStringToObject(request.get(), "end", end.c_str());
        out_list.period = label;
    }

    ESP_LOGI(TAG, "Listing finance transactions for %s (limit %d)",
             period.empty() ? "all time" : period.c_str(), limit);

    CJsonUniquePtr response;
    if (!CallAppsScript(request.get(), response, out_error)) {
        return false;
    }

    auto transactions = cJSON_GetObjectItem(response.get(), "transactions");
    if (cJSON_IsArray(transactions)) {
        cJSON* entry;
        cJSON_ArrayForEach(entry, transactions) {
            auto id = cJSON_GetObjectItem(entry, "id");
            auto date = cJSON_GetObjectItem(entry, "date");
            auto amount = cJSON_GetObjectItem(entry, "amount");
            auto category = cJSON_GetObjectItem(entry, "category");
            auto note = cJSON_GetObjectItem(entry, "note");

            FinanceTransaction transaction;
            transaction.id = cJSON_IsString(id) ? id->valuestring : "";
            transaction.date = cJSON_IsString(date) ? date->valuestring : "";
            transaction.amount =
                cJSON_IsNumber(amount) ? static_cast<long long>(amount->valuedouble) : 0;
            transaction.category = cJSON_IsString(category) ? category->valuestring : "";
            transaction.description = cJSON_IsString(note) ? note->valuestring : "";
            out_list.transactions.push_back(std::move(transaction));
        }
    }
    return true;
}

bool FinanceService::GetSummary(const std::string& period, FinanceSummary& out_summary,
                                 std::string& out_error) {
    std::string start, end, label;
    if (!ResolvePeriodRange(period, start, end, label)) {
        out_error = "Device clock is not synced yet; try again in a moment";
        return false;
    }
    return GetSummaryForRange(start, end, label, out_summary, out_error);
}

bool FinanceService::GetCategorySummary(const std::string& period,
                                         FinanceCategorySummary& out_summary,
                                         std::string& out_error) {
    std::string start, end, label;
    if (!ResolvePeriodRange(period, start, end, label)) {
        out_error = "Device clock is not synced yet; try again in a moment";
        return false;
    }
    return GetCategorySummaryForRange(start, end, label, out_summary, out_error);
}

bool FinanceService::SetBudget(const std::string& category, long long limit,
                                std::string& out_error) {
    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate finance request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "token", kFinanceApiSecret);
    cJSON_AddStringToObject(request.get(), "action", "set_budget");
    cJSON_AddStringToObject(request.get(), "category", category.c_str());
    cJSON_AddNumberToObject(request.get(), "limit", static_cast<double>(limit));

    ESP_LOGI(TAG, "Setting budget for %s to %lld", category.c_str(), limit);

    CJsonUniquePtr response;
    return CallAppsScript(request.get(), response, out_error);
}

bool FinanceService::GetBudgetStatus(const std::string& category, const std::string& period,
                                      FinanceBudgetStatus& out_status, std::string& out_error) {
    std::string start, end, label;
    if (!ResolvePeriodRange(period, start, end, label)) {
        out_error = "Device clock is not synced yet; try again in a moment";
        return false;
    }

    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate finance request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "token", kFinanceApiSecret);
    cJSON_AddStringToObject(request.get(), "action", "get_budget");
    cJSON_AddStringToObject(request.get(), "category", category.c_str());
    cJSON_AddStringToObject(request.get(), "start", start.c_str());
    cJSON_AddStringToObject(request.get(), "end", end.c_str());

    ESP_LOGI(TAG, "Getting budget status for %s (%s..%s)", category.c_str(), start.c_str(),
             end.c_str());

    CJsonUniquePtr response;
    if (!CallAppsScript(request.get(), response, out_error)) {
        return false;
    }

    auto category_item = cJSON_GetObjectItem(response.get(), "category");
    auto budget = cJSON_GetObjectItem(response.get(), "budget");
    auto spent = cJSON_GetObjectItem(response.get(), "spent");
    auto remaining = cJSON_GetObjectItem(response.get(), "remaining");
    auto percentage = cJSON_GetObjectItem(response.get(), "percentage");
    auto status = cJSON_GetObjectItem(response.get(), "status");

    out_status.category = cJSON_IsString(category_item) ? category_item->valuestring : category;
    out_status.budget = cJSON_IsNumber(budget) ? static_cast<long long>(budget->valuedouble) : 0;
    out_status.spent = cJSON_IsNumber(spent) ? static_cast<long long>(spent->valuedouble) : 0;
    out_status.remaining =
        cJSON_IsNumber(remaining) ? static_cast<long long>(remaining->valuedouble) : 0;
    out_status.percentage = cJSON_IsNumber(percentage) ? percentage->valuedouble : 0;
    out_status.status = cJSON_IsString(status) ? status->valuestring : "";
    return true;
}

bool FinanceService::ComparePeriods(const std::string& period_a, const std::string& period_b,
                                     const std::string& category,
                                     FinancePeriodComparison& out_comparison,
                                     std::string& out_error) {
    std::string start_a, end_a, label_a;
    if (!ResolvePeriodRange(period_a, start_a, end_a, label_a)) {
        out_error = "Device clock is not synced yet; try again in a moment";
        return false;
    }

    std::string start_b, end_b, label_b;
    bool resolved_b = period_b.empty() ? ResolvePreviousPeriodRange(period_a, start_b, end_b, label_b)
                                        : ResolvePeriodRange(period_b, start_b, end_b, label_b);
    if (!resolved_b) {
        out_error = "Device clock is not synced yet; try again in a moment";
        return false;
    }

    long long value_a = 0;
    long long value_b = 0;

    if (category.empty()) {
        FinanceSummary summary_a, summary_b;
        if (!GetSummaryForRange(start_a, end_a, label_a, summary_a, out_error)) return false;
        if (!GetSummaryForRange(start_b, end_b, label_b, summary_b, out_error)) return false;
        value_a = summary_a.expense;
        value_b = summary_b.expense;
    } else {
        FinanceCategorySummary categories_a, categories_b;
        if (!GetCategorySummaryForRange(start_a, end_a, label_a, categories_a, out_error)) {
            return false;
        }
        if (!GetCategorySummaryForRange(start_b, end_b, label_b, categories_b, out_error)) {
            return false;
        }
        // Categories are signed (positive income, negative expense -- see apps_script.gs'
        // normalizeAmount), but "so sanh tien an thang nay va thang truoc" cares about the
        // magnitude of food spending, not the ledger sign; abs() keeps "more" mean a bigger
        // number regardless of whether `category` happens to be an income or expense one.
        for (const auto& item : categories_a.categories) {
            if (item.category == category) value_a = item.total < 0 ? -item.total : item.total;
        }
        for (const auto& item : categories_b.categories) {
            if (item.category == category) value_b = item.total < 0 ? -item.total : item.total;
        }
    }

    out_comparison.period_a = label_a;
    out_comparison.period_b = label_b;
    out_comparison.category = category;
    out_comparison.value_a = value_a;
    out_comparison.value_b = value_b;
    out_comparison.difference = value_a - value_b;
    double denom = value_b < 0 ? -static_cast<double>(value_b) : static_cast<double>(value_b);
    out_comparison.percent_change =
        denom != 0 ? static_cast<double>(value_a - value_b) / denom * 100.0 : 0.0;
    return true;
}

namespace {

void ParseSavingsGoal(cJSON* response, FinanceSavingsGoal& out_goal) {
    auto target = cJSON_GetObjectItem(response, "target");
    auto saved = cJSON_GetObjectItem(response, "saved");
    auto remaining = cJSON_GetObjectItem(response, "remaining");
    auto percentage = cJSON_GetObjectItem(response, "percentage");
    auto completed = cJSON_GetObjectItem(response, "completed");
    auto deadline = cJSON_GetObjectItem(response, "deadline");
    auto completed_date = cJSON_GetObjectItem(response, "completed_date");

    out_goal.target = cJSON_IsNumber(target) ? static_cast<long long>(target->valuedouble) : 0;
    out_goal.saved = cJSON_IsNumber(saved) ? static_cast<long long>(saved->valuedouble) : 0;
    out_goal.remaining =
        cJSON_IsNumber(remaining) ? static_cast<long long>(remaining->valuedouble) : 0;
    out_goal.percentage = cJSON_IsNumber(percentage) ? percentage->valuedouble : 0;
    out_goal.completed = cJSON_IsTrue(completed);
    out_goal.deadline = cJSON_IsString(deadline) ? deadline->valuestring : "";
    out_goal.completed_date = cJSON_IsString(completed_date) ? completed_date->valuestring : "";
}

}  // namespace

bool FinanceService::AddSavingsGoal(const std::string& name, long long target_amount,
                                     const std::string& deadline, std::string& out_error) {
    if (name.empty()) {
        out_error = "Missing savings goal name";
        return false;
    }

    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate finance request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "token", kFinanceApiSecret);
    cJSON_AddStringToObject(request.get(), "action", "add_savings_goal");
    cJSON_AddStringToObject(request.get(), "name", name.c_str());
    cJSON_AddNumberToObject(request.get(), "target_amount", static_cast<double>(target_amount));
    cJSON_AddStringToObject(request.get(), "deadline", deadline.c_str());

    ESP_LOGI(TAG, "Creating savings goal %s: target %lld, deadline %s", name.c_str(),
             target_amount, deadline.empty() ? "none" : deadline.c_str());

    CJsonUniquePtr response;
    return CallAppsScript(request.get(), response, out_error);
}

bool FinanceService::GetSavingsGoal(const std::string& name, FinanceSavingsGoal& out_goal,
                                     std::string& out_error) {
    if (name.empty()) {
        out_error = "Missing savings goal name";
        return false;
    }

    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate finance request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "token", kFinanceApiSecret);
    cJSON_AddStringToObject(request.get(), "action", "get_savings_goal");
    cJSON_AddStringToObject(request.get(), "name", name.c_str());

    ESP_LOGI(TAG, "Getting savings goal %s", name.c_str());

    CJsonUniquePtr response;
    if (!CallAppsScript(request.get(), response, out_error)) {
        return false;
    }

    out_goal.name = name;
    ParseSavingsGoal(response.get(), out_goal);
    return true;
}

bool FinanceService::ListSavingsGoals(FinanceSavingsGoalList& out_list, std::string& out_error) {
    out_list.goals.clear();

    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate finance request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "token", kFinanceApiSecret);
    cJSON_AddStringToObject(request.get(), "action", "list_savings_goals");

    ESP_LOGI(TAG, "Listing savings goals");

    CJsonUniquePtr response;
    if (!CallAppsScript(request.get(), response, out_error)) {
        return false;
    }

    auto goals = cJSON_GetObjectItem(response.get(), "goals");
    if (cJSON_IsArray(goals)) {
        cJSON* entry;
        cJSON_ArrayForEach(entry, goals) {
            auto name = cJSON_GetObjectItem(entry, "name");
            FinanceSavingsGoal goal;
            goal.name = cJSON_IsString(name) ? name->valuestring : "";
            ParseSavingsGoal(entry, goal);
            out_list.goals.push_back(std::move(goal));
        }
    }
    return true;
}

bool FinanceService::DeleteSavingsGoal(const std::string& name, std::string& out_error) {
    if (name.empty()) {
        out_error = "Missing savings goal name";
        return false;
    }

    CJsonUniquePtr request(cJSON_CreateObject());
    if (!request) {
        out_error = "Failed to allocate finance request";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "token", kFinanceApiSecret);
    cJSON_AddStringToObject(request.get(), "action", "delete_savings_goal");
    cJSON_AddStringToObject(request.get(), "name", name.c_str());

    ESP_LOGI(TAG, "Deleting savings goal %s", name.c_str());

    CJsonUniquePtr response;
    return CallAppsScript(request.get(), response, out_error);
}
