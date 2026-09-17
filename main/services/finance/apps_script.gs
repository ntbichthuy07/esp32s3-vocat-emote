// Google Apps Script Web App backing the ESP32 self.finance.* MCP tools.
//
// Setup:
//   1. Create a Google Sheet. Add a first row of headers to the "Transactions" sheet (or rename
//      Sheet1 to "Transactions"): ID | Date | Amount | Category | Note
//   2. Extensions > Apps Script, delete the sample code, paste this whole file in as Code.gs.
//   3. Change SHARED_SECRET below to a random string of your own.
//   4. Deploy > New deployment > type "Web app". Execute as: Me. Who has access: Anyone.
//   5. Copy services/service_config.example.h to services/service_config.h (gitignored), then
//      put the resulting URL (ends in /exec) into kFinanceApiUrl there, and the same
//      SHARED_SECRET into kFinanceApiSecret.
//   6. Any time you edit this file, go to Deploy > Manage deployments > pencil icon > Version:
//      "New version" > Deploy -- saving in the editor alone does not update the live /exec URL.
//
// All date filtering here is done by the caller (the ESP32 firmware) resolving a period keyword
// like "today"/"week"/"month" into an explicit [start, end) date range in its own timezone, so
// this script only ever compares plain "YYYY-MM-DD" strings -- it doesn't need to know the
// device's timezone or today's date itself.
//
// Budgets live in the same "Transactions" sheet, columns F (Category) and G (MonthlyLimit) --
// one row per category with a budget set via self.finance.set_budget, independent of how many
// transaction rows exist in A:E. The F:G header is written automatically on first use, no manual
// setup needed.

var SHARED_SECRET = "CHANGE_ME";
var SHEET_NAME = "Transactions";
var BUDGET_CATEGORY_COL = 6;   // F
var BUDGET_LIMIT_COL = 7;      // G
var MAX_LIST_LIMIT = 30;
var BUDGET_WARNING_THRESHOLD = 0.8;   // >= 80% spent
var BUDGET_EXCEEDED_THRESHOLD = 1.0;  // >= 100% spent

// "other" is shared between the two; everything else is unambiguously expense or income, which
// lets addTransaction/updateTransaction auto-correct the amount's sign to match the category.
var EXPENSE_CATEGORIES = ["food", "transport", "shopping", "housing", "utilities", "health",
                           "entertainment", "education", "travel", "subscriptions", "family",
                           "personal"];
var INCOME_CATEGORIES = ["salary", "bonus", "freelance", "investment", "gift", "refund"];
var ALLOWED_CATEGORIES = EXPENSE_CATEGORIES.concat(INCOME_CATEGORIES).concat(["other"]);

function doPost(e) {
  try {
    var body = JSON.parse(e.postData.contents);
    if (body.token !== SHARED_SECRET) {
      return jsonResponse({ok: false, error: "invalid token"});
    }

    var sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(SHEET_NAME);
    if (!sheet) {
      return jsonResponse({ok: false, error: "sheet '" + SHEET_NAME + "' not found"});
    }

    switch (body.action) {
      case "add_transaction":
        return addTransaction(sheet, body);
      case "update_transaction":
        return updateTransaction(sheet, body);
      case "delete_transaction":
        return deleteTransaction(sheet, body);
      case "list_transactions":
        return listTransactions(sheet, body);
      case "get_summary":
        return getSummary(sheet, body);
      case "get_category_summary":
        return getCategorySummary(sheet, body);
      case "set_budget":
        return setBudget(sheet, body);
      case "get_budget":
        return getBudget(sheet, body);
      default:
        return jsonResponse({ok: false, error: "unknown action: " + body.action});
    }
  } catch (err) {
    return jsonResponse({ok: false, error: String(err)});
  }
}

// Columns: A=ID, B=Date, C=Amount, D=Category, E=Note. F=BudgetCategory, G=MonthlyLimit hold
// the budget rows (see findBudgetRow below).

// Falls back to "other" for anything not in ALLOWED_CATEGORIES (including old free-text
// categories from before this list existed), so category summaries never fragment into one-off
// labels the device's tool description doesn't know about.
function normalizeCategory(category) {
  var normalized = String(category || "").trim().toLowerCase();
  return ALLOWED_CATEGORIES.indexOf(normalized) !== -1 ? normalized : "other";
}

// Forces the sign to match the category (income -> positive, expense -> negative, mirroring a
// bank statement) so a mis-signed amount from the caller can't silently corrupt income/expense
// totals. "other" is ambiguous, so its sign is left as given.
function normalizeAmount(amount, category) {
  var value = Number(amount) || 0;
  if (EXPENSE_CATEGORIES.indexOf(category) !== -1) return -Math.abs(value);
  if (INCOME_CATEGORIES.indexOf(category) !== -1) return Math.abs(value);
  return value;
}

function addTransaction(sheet, body) {
  var id = generateId();
  var category = normalizeCategory(body.category);
  var amount = normalizeAmount(body.amount, category);
  sheet.appendRow([id, new Date(body.date), amount, category, body.note || ""]);
  return jsonResponse({ok: true, id: id, category: category, amount: amount});
}

function updateTransaction(sheet, body) {
  var id = String(body.id || "");
  if (!id) {
    return jsonResponse({ok: false, error: "missing id"});
  }
  var data = sheet.getDataRange().getValues();
  for (var i = 1; i < data.length; i++) {
    if (String(data[i][0]) === id) {
      var category = normalizeCategory(body.category);
      var amount = normalizeAmount(body.amount, category);
      var row = i + 1;  // +1: getValues() is 0-indexed, sheet rows are 1-indexed
      sheet.getRange(row, 3, 1, 3).setValues([[amount, category, body.note || ""]]);
      return jsonResponse({ok: true, id: id, category: category, amount: amount});
    }
  }
  return jsonResponse({ok: false, error: "transaction not found: " + id});
}

function deleteTransaction(sheet, body) {
  var id = String(body.id || "");
  if (!id) {
    return jsonResponse({ok: false, error: "missing id"});
  }
  var data = sheet.getDataRange().getValues();
  for (var i = 1; i < data.length; i++) {
    if (String(data[i][0]) === id) {
      sheet.deleteRow(i + 1);  // +1: getValues() is 0-indexed, sheet rows are 1-indexed
      return jsonResponse({ok: true, deleted: true});
    }
  }
  return jsonResponse({ok: false, error: "transaction not found: " + id});
}

// `start`/`end` are "YYYY-MM-DD" strings forming a half-open range, or both empty for no filter.
function listTransactions(sheet, body) {
  var start = String(body.start || "");
  var end = String(body.end || "");
  var limit = Math.min(Math.max(parseInt(body.limit, 10) || 10, 1), MAX_LIST_LIMIT);
  var data = sheet.getDataRange().getValues();
  var rows = [];
  for (var i = 1; i < data.length; i++) {
    var date = data[i][1];
    if (!(date instanceof Date)) continue;
    var isoDateTime = isoDateTimeOf(date);
    var isoDate = isoDateTime.substring(0, 10);
    if (start && isoDate < start) continue;
    if (end && isoDate >= end) continue;
    rows.push({
      id: String(data[i][0]),
      date: isoDateTime,
      amount: Number(data[i][2]) || 0,
      category: String(data[i][3] || ""),
      note: String(data[i][4] || "")
    });
  }
  rows.sort(function(a, b) { return a.date < b.date ? 1 : -1; });  // most recent first
  return jsonResponse({ok: true, transactions: rows.slice(0, limit)});
}

// `start`/`end` are "YYYY-MM-DD" strings forming a half-open range.
function getSummary(sheet, body) {
  var start = String(body.start || "");
  var end = String(body.end || "");
  var data = sheet.getDataRange().getValues();
  var income = 0;
  var expense = 0;
  var count = 0;
  for (var i = 1; i < data.length; i++) {
    var date = data[i][1];
    if (!(date instanceof Date)) continue;
    var isoDate = isoDateTimeOf(date).substring(0, 10);
    if (isoDate < start || isoDate >= end) continue;
    var amount = Number(data[i][2]) || 0;
    if (amount >= 0) {
      income += amount;
    } else {
      expense += -amount;
    }
    count++;
  }
  return jsonResponse({ok: true, income: income, expense: expense, net: income - expense,
                        count: count});
}

// `start`/`end` are "YYYY-MM-DD" strings forming a half-open range.
function getCategorySummary(sheet, body) {
  var start = String(body.start || "");
  var end = String(body.end || "");
  var data = sheet.getDataRange().getValues();
  var totals = {};  // category -> {total, count}
  var order = [];
  for (var i = 1; i < data.length; i++) {
    var date = data[i][1];
    if (!(date instanceof Date)) continue;
    var isoDate = isoDateTimeOf(date).substring(0, 10);
    if (isoDate < start || isoDate >= end) continue;
    var category = String(data[i][3] || "other");
    if (!totals[category]) {
      totals[category] = {total: 0, count: 0};
      order.push(category);
    }
    totals[category].total += Number(data[i][2]) || 0;
    totals[category].count++;
  }
  var categories = order.map(function(category) {
    return {category: category, total: totals[category].total, count: totals[category].count};
  });
  categories.sort(function(a, b) { return Math.abs(b.total) - Math.abs(a.total); });
  return jsonResponse({ok: true, categories: categories});
}

// Budget columns F (Category) and G (MonthlyLimit) on the Transactions sheet, one row per
// category, laid out independently of how far column A's transaction rows currently reach.
// Returns {row, nextRow}: `row` is the existing row for `category` (-1 if not set yet), and
// `nextRow` is where a new category should be appended (right after the last budget row found,
// not tied to the sheet's overall last row -- so adding a budget never skips down to wherever
// the transaction rows happen to end).
function findBudgetRow(sheet, category) {
  var lastRow = sheet.getLastRow();
  if (lastRow < 2) return {row: -1, nextRow: 2};
  var values = sheet.getRange(2, BUDGET_CATEGORY_COL, lastRow - 1, 1).getValues();
  var lastBudgetRow = 1;  // header row = "no budget rows yet"
  for (var i = 0; i < values.length; i++) {
    var cell = String(values[i][0] || "");
    if (cell === category) return {row: i + 2, nextRow: -1};
    if (cell !== "") lastBudgetRow = i + 2;
  }
  return {row: -1, nextRow: lastBudgetRow + 1};
}

function ensureBudgetHeaders(sheet) {
  var header = sheet.getRange(1, BUDGET_CATEGORY_COL, 1, 2).getValues()[0];
  if (header[0] !== "BudgetCategory" || header[1] !== "MonthlyLimit") {
    sheet.getRange(1, BUDGET_CATEGORY_COL, 1, 2).setValues([["BudgetCategory", "MonthlyLimit"]]);
  }
}

function setBudget(sheet, body) {
  var category = normalizeCategory(body.category);
  var limit = Number(body.limit) || 0;
  ensureBudgetHeaders(sheet);
  var found = findBudgetRow(sheet, category);
  if (found.row !== -1) {
    sheet.getRange(found.row, BUDGET_LIMIT_COL).setValue(limit);
  } else {
    sheet.getRange(found.nextRow, BUDGET_CATEGORY_COL, 1, 2).setValues([[category, limit]]);
  }
  return jsonResponse({ok: true, category: category, limit: limit});
}

// `start`/`end` are "YYYY-MM-DD" strings forming a half-open range over the Transactions sheet.
function getBudget(sheet, body) {
  var category = normalizeCategory(body.category);
  var start = String(body.start || "");
  var end = String(body.end || "");

  var found = findBudgetRow(sheet, category);
  if (found.row === -1) {
    return jsonResponse({ok: false, error: "no budget set for category: " + category});
  }
  var limit = Number(sheet.getRange(found.row, BUDGET_LIMIT_COL).getValue()) || 0;

  // Expense categories are stored negative (see normalizeAmount) -- "spent" is the magnitude.
  var spent = Math.abs(categoryTotalInRange(sheet, category, start, end));
  var remaining = limit - spent;
  var percentage = limit > 0 ? (spent / limit) * 100 : 0;
  var status = percentage >= BUDGET_EXCEEDED_THRESHOLD * 100 ? "exceeded" :
               percentage >= BUDGET_WARNING_THRESHOLD * 100 ? "warning" : "ok";
  return jsonResponse({ok: true, category: category, budget: limit, spent: spent,
                        remaining: remaining, percentage: percentage, status: status});
}

// Sums a single category's transactions within a half-open "YYYY-MM-DD" date range.
function categoryTotalInRange(sheet, category, start, end) {
  var data = sheet.getDataRange().getValues();
  var total = 0;
  for (var i = 1; i < data.length; i++) {
    var date = data[i][1];
    if (!(date instanceof Date)) continue;
    if (String(data[i][3] || "other") !== category) continue;
    var isoDate = isoDateTimeOf(date).substring(0, 10);
    if (isoDate < start || isoDate >= end) continue;
    total += Number(data[i][2]) || 0;
  }
  return total;
}

function generateId() {
  return Date.now().toString(36) + Math.floor(Math.random() * 46656).toString(36);
}

// Formats a Date as "yyyy-MM-ddTHH:mm:ss" in the spreadsheet's own timezone, so it round-trips
// consistently regardless of where the script happens to run.
function isoDateTimeOf(date) {
  return Utilities.formatDate(date, Session.getScriptTimeZone(), "yyyy-MM-dd'T'HH:mm:ss");
}

function jsonResponse(obj) {
  return ContentService.createTextOutput(JSON.stringify(obj))
      .setMimeType(ContentService.MimeType.JSON);
}
