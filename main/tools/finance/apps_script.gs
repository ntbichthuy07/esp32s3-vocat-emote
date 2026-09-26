// Google Apps Script Web App backing the ESP32 self.finance.* MCP tools.
//
// Setup:
//   1. Create a Google Sheet. Add a first row of headers to the "Transactions" sheet (or rename
//      Sheet1 to "Transactions"): ID | Date | Note | Amount | Category
//   2. Extensions > Apps Script, delete the sample code, paste this whole file in as Code.gs.
//   3. Change SHARED_SECRET below to a random string of your own.
//   4. Deploy > New deployment > type "Web app". Execute as: Me. Who has access: Anyone.
//   5. Copy tools/service_config.example.h to tools/service_config.h (gitignored), then
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
//
// Funding goals live in the same sheet too, columns J (ID) through P (CompletedDate) -- one row
// per goal, independent of A:E and F:G, header auto-written on first use like the budget one. A
// funding goal covers both directions of "money set aside towards a named target": a savings
// goal (accumulating towards a purchase, logged via the `savings` transaction category) or a
// debt-payoff goal (paying one down, logged via `debt`) -- see kCategoryHelp in
// finance_mcp_tool.cc. SavedAmount (column M) is a live formula written once by addFundingGoal
// (see fundingGoalSavedFormula/FUNDING_GOAL_SAVED) that sums every `savings`/`debt` transaction
// whose Note contains this goal's ID (column J, e.g. "F1") as a whole word -- it doesn't
// distinguish which of the two categories a match came from, so a savings goal and a debt goal
// referencing the same transaction would double-count it, but since matching is by each goal's
// own unique ID rather than by name, that only happens if the same ID is put in a note on
// purpose. It stays in sync automatically; overwriting the cell by hand replaces the formula
// with a static number. A goal is "completed" once SavedAmount >= TargetAmount (derived fresh on
// every read, not stored).
// CompletedDate (column P) is kept in sync with that by getFundingGoal/listFundingGoals (see
// syncCompletedDate): stamped with today's date the moment a goal is observed completed, and
// cleared back to blank if a later correction (e.g. to a matching transaction, which
// SavedAmount's formula reacts to automatically) drops saved back below target. Deleting a
// goal clears only its J:P cells (never a full-row delete, which would also wipe out whatever
// unrelated transaction or budget happens to share that row index).

var SHARED_SECRET = "CHANGE_ME";
var SHEET_NAME = "Transactions";
var BUDGET_CATEGORY_COL = 6;   // F
var BUDGET_LIMIT_COL = 7;      // G
var GOAL_ID_COL = 10;          // J
var GOAL_NAME_COL = 11;        // K
var GOAL_TARGET_COL = 12;      // L
var GOAL_SAVED_COL = 13;       // M
var GOAL_DEADLINE_COL = 14;    // N
var GOAL_CREATED_COL = 15;     // O
var GOAL_COMPLETED_DATE_COL = 16;  // P
var MAX_LIST_LIMIT = 30;
var BUDGET_WARNING_THRESHOLD = 0.8;   // >= 80% spent
var BUDGET_EXCEEDED_THRESHOLD = 1.0;  // >= 100% spent

// "other" is shared between the two; everything else here is unambiguously expense or income,
// which lets addTransaction/updateTransaction auto-correct the amount's sign to match the
// category.
var EXPENSE_CATEGORIES = ["food", "transport", "shopping", "housing", "utilities", "health",
                           "entertainment", "education", "travel", "subscriptions", "family",
                           "personal"];
var INCOME_CATEGORIES = ["salary", "bonus", "freelance", "investment", "gift", "refund"];
// "savings" and "debt" are bidirectional, unlike every category above: depositing towards a
// funding goal (see the FundingGoal columns below) is negative (money leaving spendable cash),
// but withdrawing from one -- pulling saved money back out, or reversing a debt payment -- is
// positive (money coming back in). So neither list above claims them, and normalizeAmount leaves
// their sign exactly as the caller sent it (like "other"), trusting finance_mcp_tool.cc's
// kCategoryHelp to have told the LLM which sign means which direction.
var FUNDING_CATEGORIES = ["savings", "debt"];
var ALLOWED_CATEGORIES = EXPENSE_CATEGORIES.concat(INCOME_CATEGORIES).concat(FUNDING_CATEGORIES)
    .concat(["other"]);

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
      case "add_funding_goal":
        return addFundingGoal(sheet, body);
      case "get_funding_goal":
        return getFundingGoal(sheet, body);
      case "list_funding_goals":
        return listFundingGoals(sheet, body);
      case "delete_funding_goal":
        return deleteFundingGoal(sheet, body);
      default:
        return jsonResponse({ok: false, error: "unknown action: " + body.action});
    }
  } catch (err) {
    return jsonResponse({ok: false, error: String(err)});
  }
}

// Columns: A=ID, B=Date, C=Note, D=Amount, E=Category. F=BudgetCategory, G=MonthlyLimit hold
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
// totals. "other" is ambiguous, and "savings"/"debt" are intentionally bidirectional (see
// FUNDING_CATEGORIES above), so both are left exactly as given.
function normalizeAmount(amount, category) {
  var value = Number(amount) || 0;
  if (EXPENSE_CATEGORIES.indexOf(category) !== -1) return -Math.abs(value);
  if (INCOME_CATEGORIES.indexOf(category) !== -1) return Math.abs(value);
  return value;
}

// Base36 timestamp (millisecond resolution) plus a short random suffix, so ids sort roughly by
// creation time while staying unique even when two transactions are added within the same
// millisecond. Unlike the old "scan column A for the max integer, then +1" scheme, this needs no
// read of the sheet before appending, so concurrent Apps Script executions can't race each other
// into computing the same next id.
function generateTransactionId() {
  return Date.now().toString(36) + Math.random().toString(36).substring(2, 6);
}

function addTransaction(sheet, body) {
  var id = generateTransactionId();
  var category = normalizeCategory(body.category);
  var amount = normalizeAmount(body.amount, category);
  // The caller sends a full ISO datetime (see ResolveDate in finance_service.cc), but the
  // ledger only tracks which day a transaction happened on, not the time -- zero out the time
  // of day before storing, and format the cell as a plain date so it doesn't display "00:00:00".
  var date = new Date(body.date);
  date.setHours(0, 0, 0, 0);
  sheet.appendRow([id, date, body.note || "", amount, category]);
  sheet.getRange(sheet.getLastRow(), 2).setNumberFormat("yyyy-mm-dd");
  return jsonResponse({ok: true, id: String(id), category: category, amount: amount});
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
      sheet.getRange(row, 3, 1, 3).setValues([[body.note || "", amount, category]]);
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
      note: String(data[i][2] || ""),
      amount: Number(data[i][3]) || 0,
      category: String(data[i][4] || "")
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
    var amount = Number(data[i][3]) || 0;
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
    var category = String(data[i][4] || "other");
    if (!totals[category]) {
      totals[category] = {total: 0, count: 0};
      order.push(category);
    }
    totals[category].total += Number(data[i][3]) || 0;
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
    if (String(data[i][4] || "other") !== category) continue;
    var isoDate = isoDateTimeOf(date).substring(0, 10);
    if (isoDate < start || isoDate >= end) continue;
    total += Number(data[i][3]) || 0;
  }
  return total;
}

// Funding goal columns J (ID) through P (CompletedDate) on the Transactions sheet, one row per
// goal, laid out independently of the transaction rows in A:E and the budget rows in F:G.
// Returns {row, nextRow}: `row` is the existing row for `name` (-1 if no such goal), and
// `nextRow` is where a new goal should be appended (right after the last goal row found, not
// tied to the sheet's overall last row).
function findGoalRow(sheet, name) {
  var lastRow = sheet.getLastRow();
  if (lastRow < 2) return {row: -1, nextRow: 2};
  var values = sheet.getRange(2, GOAL_NAME_COL, lastRow - 1, 1).getValues();
  var lastGoalRow = 1;  // header row = "no goal rows yet"
  for (var i = 0; i < values.length; i++) {
    var cell = String(values[i][0] || "");
    if (cell === name) return {row: i + 2, nextRow: -1};
    if (cell !== "") lastGoalRow = i + 2;
  }
  return {row: -1, nextRow: lastGoalRow + 1};
}

function ensureGoalHeaders(sheet) {
  var expected = ["ID", "FundingGoal", "TargetAmount", "SavedAmount", "Deadline", "CreatedDate",
                   "CompletedDate"];
  var header = sheet.getRange(1, GOAL_ID_COL, 1, expected.length).getValues()[0];
  for (var i = 0; i < expected.length; i++) {
    if (header[i] !== expected[i]) {
      sheet.getRange(1, GOAL_ID_COL, 1, expected.length).setValues([expected]);
      return;
    }
  }
}

// Converts a 1-based column index (<=26) to its A1 letter, e.g. 3 -> "C". Used below to build a
// formula without hardcoding letters that would drift if a column ever moves.
function columnLetter(col) {
  return String.fromCharCode(64 + col);
}

// Lowercases and strips Vietnamese diacritics (accent marks), e.g. "Mua Xe Máy" -> "mua xe may".
// Applied to both a goal's ID and a transaction's Note before the word-boundary match in
// FUNDING_GOAL_SAVED, so a case difference (a note typed as "f1" instead of "F1") doesn't cause
// an otherwise-exact match to fail -- goal IDs are always plain ASCII (e.g. "F1"), so the
// diacritic-stripping itself is mostly moot for them, but harmless, and keeps this one helper
// generic enough to reuse if matching ever needs to consider accented text again.
function normalizeForMatch(text) {
  return String(text || "")
      .toLowerCase()
      .normalize("NFD")
      .replace(/\p{Mark}/gu, "")  // combining accent marks left behind by NFD
      .replace(/đ/g, "d");        // "đ" doesn't decompose under NFD, unlike accented vowels
}

// Sums every transaction whose Category is `savings` or `debt` (see FUNDING_CATEGORIES) and
// whose Note contains `goalId` (e.g. "F1") as a whole token, then flips the sign: a deposit is
// stored negative (money leaving spendable cash) and a withdrawal positive (money coming back --
// see FUNDING_CATEGORIES/normalizeAmount in doPost), so summing them nets deposits against
// withdrawals, and flipping that net gives a positive "progress so far" -- this is the live
// formula behind a funding goal's SavedAmount (column M), built by fundingGoalSavedFormula
// below. There's no per-goal Type to filter on (a goal isn't tagged savings-only or debt-only),
// so this doesn't distinguish which of the two categories a match came from -- that's fine in
// practice since matching is by each goal's own unique ID rather than by name, so a transaction
// only counts towards a goal if its Note deliberately names that exact ID. Matching against the
// goal's ID (see GOAL_ID_COL, addFundingGoal) rather than its name is deliberate: an ID like
// "F1" is short and not something a user would otherwise say, so the LLM can drop it into a Note
// reliably, unlike reliably working an exact name into natural language. The match uses a `\b`
// word-boundary regex, not a plain substring check, specifically so "F1" doesn't also match
// inside "F10"-"F19" or "F100"-"F199" etc. `categories`/`notes`/`amounts` must be passed as
// explicit range arguments (not read via SpreadsheetApp internally) so Sheets' dependency
// tracking knows to recalculate this cell whenever the Transactions data changes.
/**
 * @param {string} goalId
 * @param {Array<Array<string>>} categories
 * @param {Array<Array<string>>} notes
 * @param {Array<Array<number>>} amounts
 * @return {number}
 * @customfunction
 */
function FUNDING_GOAL_SAVED(goalId, categories, notes, amounts) {
  var id = normalizeForMatch(goalId);
  if (!id) return 0;
  var idPattern = new RegExp("\\b" + id + "\\b");
  var total = 0;
  for (var i = 0; i < notes.length; i++) {
    if (FUNDING_CATEGORIES.indexOf(String(categories[i][0] || "")) === -1) continue;
    if (!idPattern.test(normalizeForMatch(notes[i][0]))) continue;
    total += Number(amounts[i][0]) || 0;
  }
  return -total;
}

// Builds the FUNDING_GOAL_SAVED formula for a newly created goal row (see above). `row` is that
// goal's own row, so the id reference ($J<row>) always points back at itself.
function fundingGoalSavedFormula(row) {
  var idCol = columnLetter(GOAL_ID_COL);
  var categoryCol = columnLetter(5);  // E, Category
  var noteCol = columnLetter(3);      // C, Note
  var amountCol = columnLetter(4);    // D, Amount
  var categoryRange = "$" + categoryCol + "$2:$" + categoryCol + "$100000";
  var noteRange = "$" + noteCol + "$2:$" + noteCol + "$100000";
  var amountRange = "$" + amountCol + "$2:$" + amountCol + "$100000";
  return "=FUNDING_GOAL_SAVED($" + idCol + row + "," + categoryRange + "," + noteRange + "," +
      amountRange + ")";
}

function addFundingGoal(sheet, body) {
  var name = String(body.name || "").trim();
  if (!name) {
    return jsonResponse({ok: false, error: "missing name"});
  }
  ensureGoalHeaders(sheet);
  var found = findGoalRow(sheet, name);
  if (found.row !== -1) {
    return jsonResponse({ok: false, error: "funding goal already exists: " + name});
  }
  var target = Number(body.target_amount) || 0;
  var deadline = String(body.deadline || "");
  // A formula ("F1", "F2", ...), not a computed-once value -- safe here (unlike a transaction's
  // ID, see addTransaction) because deleteFundingGoal only ever clearContent()s a row rather than
  // removing it, so a goal's row -- and therefore its ROW()-based ID -- never shifts once set.
  var idFormula = '="F"&(ROW()-1)';
  var id = "F" + (found.nextRow - 1);
  var created = isoDateTimeOf(new Date()).substring(0, 10);
  sheet.getRange(found.nextRow, GOAL_ID_COL, 1, 7)
      .setValues([[idFormula, name, target, fundingGoalSavedFormula(found.nextRow), deadline,
                    created, ""]]);
  return jsonResponse({ok: true, id: id, name: name, target: target, deadline: deadline});
}

// Keeps CompletedDate (column P) in sync with whether the goal is currently completed (saved >=
// target): stamps today's date in the first time it's seen completed with the cell still blank,
// and clears it back out if a later correction (e.g. editing/deleting a matching transaction,
// which SavedAmount's formula picks up automatically) has dropped saved back below target.
// Returns the resulting value so callers can include it in their response without a second read.
function syncCompletedDate(sheet, row, completed, completedDate) {
  if (completed) {
    if (completedDate) return completedDate;
    var stamped = isoDateTimeOf(new Date()).substring(0, 10);
    sheet.getRange(row, GOAL_COMPLETED_DATE_COL).setValue(stamped);
    return stamped;
  }
  if (completedDate) {
    sheet.getRange(row, GOAL_COMPLETED_DATE_COL).clearContent();
  }
  return "";
}

function getFundingGoal(sheet, body) {
  var name = String(body.name || "").trim();
  if (!name) {
    return jsonResponse({ok: false, error: "missing name"});
  }
  var found = findGoalRow(sheet, name);
  if (found.row === -1) {
    return jsonResponse({ok: false, error: "no funding goal named: " + name});
  }
  var row = sheet.getRange(found.row, GOAL_ID_COL, 1, 7).getValues()[0];
  var id = String(row[0] || "");
  var target = Number(row[2]) || 0;
  var saved = Number(row[3]) || 0;
  var deadline = String(row[4] || "");
  var completed = target > 0 && saved >= target;
  var completedDate = syncCompletedDate(sheet, found.row, completed, String(row[6] || ""));
  var percentage = target > 0 ? (saved / target) * 100 : 0;
  return jsonResponse({ok: true, id: id, name: name, target: target, saved: saved,
                        remaining: target - saved, percentage: percentage,
                        completed: completed, deadline: deadline,
                        completed_date: completedDate});
}

function listFundingGoals(sheet, body) {
  var lastRow = sheet.getLastRow();
  var goals = [];
  if (lastRow >= 2) {
    var values = sheet.getRange(2, GOAL_ID_COL, lastRow - 1, 7).getValues();
    for (var i = 0; i < values.length; i++) {
      var name = String(values[i][1] || "");
      if (!name) continue;  // cleared (deleted) goal row
      var target = Number(values[i][2]) || 0;
      var saved = Number(values[i][3]) || 0;
      var completed = target > 0 && saved >= target;
      var completedDate = syncCompletedDate(sheet, 2 + i, completed, String(values[i][6] || ""));
      goals.push({
        id: String(values[i][0] || ""),
        name: name,
        target: target,
        saved: saved,
        remaining: target - saved,
        percentage: target > 0 ? (saved / target) * 100 : 0,
        completed: completed,
        deadline: String(values[i][4] || ""),
        completed_date: completedDate
      });
    }
  }
  return jsonResponse({ok: true, goals: goals});
}

function deleteFundingGoal(sheet, body) {
  var name = String(body.name || "").trim();
  if (!name) {
    return jsonResponse({ok: false, error: "missing name"});
  }
  var found = findGoalRow(sheet, name);
  if (found.row === -1) {
    return jsonResponse({ok: false, error: "no funding goal named: " + name});
  }
  // Clears only the J:P cells for this row -- never deleteRow(), which would also delete
  // whatever unrelated transaction (A:E) or budget (F:G) happens to share this row index.
  sheet.getRange(found.row, GOAL_ID_COL, 1, 7).clearContent();
  return jsonResponse({ok: true, deleted: true});
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
