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
// Category analytics live in the same "Transactions" sheet:
// Column F: Category
// Column G: Total (formulas: G2='=SUMIF(D:D,">0",D:D)', G3='=SUM(D:D)', G4+ = '=SUMIF(E:E,"*"&F4&"*",D:D)')
// Column H: CurrentMonthly (formulas: H2=income this cycle, H3=saving this cycle, H4+=category this cycle: 25th last month to 24th this month)
//
// Categories are maintained in column F (Category): the list of supported / standardized
// categories is read dynamically from column F (from row 4 downwards). Users can add, modify,
// or customize categories directly in column F in Google Sheets.

var SHARED_SECRET = "CHANGE_ME";
var SHEET_NAME = "Transactions";
var CATEGORY_COL = 6;          // F
var TOTAL_COL = 7;             // G
var CURRENT_MONTHLY_COL = 8;   // H
var MAX_LIST_LIMIT = 30;

// Default categories (14 categories)
var DEFAULT_EXPENSE_CATEGORIES = [
  "housing", "travel", "subscriptions", "transport", "personal",
  "utilities", "food", "shopping", "family", "health"
];
var DEFAULT_INCOME_CATEGORIES = ["salary", "investment"];
var DEFAULT_FUNDING_CATEGORIES = ["debt"];
var DEFAULT_CATEGORIES = [
  "salary",
  "housing",
  "investment",
  "travel",
  "subscriptions",
  "transport",
  "personal",
  "utilities",
  "food",
  "shopping",
  "other",
  "family",
  "debt",
  "health"
];

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
      case "get_categories":
        return getCategories(sheet);
      case "get_monthly_spending":
        return getMonthlySpending(sheet, body);
      default:
        return jsonResponse({ok: false, error: "unknown action: " + body.action});
    }
  } catch (err) {
    return jsonResponse({ok: false, error: String(err)});
  }
}

// Columns: A=ID, B=Date, C=Note, D=Amount, E=Category. F=Category, G=Total, H=CurrentMonthly
// hold the categories and amount formulas.

// Reads standardized categories dynamically from column F (from row 4 downwards).
function getAllowedCategories(sheet) {
  var lastRow = sheet.getLastRow();
  if (lastRow < 4 || String(sheet.getRange(4, CATEGORY_COL).getValue()).trim() === "") {
    ensureCategoryHeaders(sheet);
    lastRow = sheet.getLastRow();
  }
  var categories = [];
  if (lastRow >= 4) {
    var values = sheet.getRange(4, CATEGORY_COL, lastRow - 3, 1).getValues();
    for (var i = 0; i < values.length; i++) {
      var cat = String(values[i][0] || "").trim();
      var catLower = cat.toLowerCase();
      if (cat !== "" && catLower !== "category" && catLower !== "budgetcategory" && categories.indexOf(cat) === -1) {
        categories.push(cat);
      }
    }
  }
  if (categories.length > 0) {
    var hasOther = false;
    for (var j = 0; j < categories.length; j++) {
      if (categories[j].toLowerCase() === "other") {
        hasOther = true;
        break;
      }
    }
    if (!hasOther) {
      categories.push("other");
    }
    return categories;
  }
  return DEFAULT_CATEGORIES;
}

function getCategories(sheet) {
  return jsonResponse({ok: true, categories: getAllowedCategories(sheet)});
}

// Falls back to "other" for anything not matching categories in column F,
// so category summaries never fragment into one-off labels the device doesn't know about.
// Matches case-insensitively and returns the category as defined in column F.
function normalizeCategory(sheet, category) {
  var input = String(category || "").trim();
  if (!input) return "other";
  var allowed = getAllowedCategories(sheet);
  var inputLower = input.toLowerCase();
  for (var i = 0; i < allowed.length; i++) {
    if (allowed[i].toLowerCase() === inputLower) {
      return allowed[i];
    }
  }
  return "other";
}

// Forces the sign to match the category:
// Known expense categories -> negative (expense)
// Known income categories -> positive (income)
// Bidirectional categories ("savings", "debt") or custom categories -> keeps the caller's sign.
function normalizeAmount(amount, category) {
  var value = Number(amount) || 0;
  var catLower = String(category || "").toLowerCase();
  if (DEFAULT_EXPENSE_CATEGORIES.indexOf(catLower) !== -1) return -Math.abs(value);
  if (DEFAULT_INCOME_CATEGORIES.indexOf(catLower) !== -1) return Math.abs(value);
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
  var category = normalizeCategory(sheet, body.category);
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
      var category = normalizeCategory(sheet, body.category);
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

// Category columns F (Category), G (Total), H (CurrentMonthly) on the Transactions sheet.
function ensureCategoryHeaders(sheet) {
  var header = sheet.getRange(1, CATEGORY_COL, 1, 3).getValues()[0];
  var colF = String(header[0] || "").trim();
  var colG = String(header[1] || "").trim();
  var colH = String(header[2] || "").trim();
  if (colF !== "Category" || colG !== "Total" || colH !== "CurrentMonthly") {
    sheet.getRange(1, CATEGORY_COL, 1, 3).setValues([["Category", "Total", "CurrentMonthly"]]);
  }
  // Clear I1 if previously set to MonthlyLimit
  if (String(sheet.getRange(1, 9).getValue()).trim().toLowerCase() === "monthlylimit") {
    sheet.getRange(1, 9).clearContent();
  }

  var f2 = String(sheet.getRange(2, CATEGORY_COL).getValue()).trim();
  if (f2.toLowerCase() !== "income") {
    sheet.getRange(2, CATEGORY_COL, 1, 2).setValues([["income", '=SUMIF(D:D,">0",D:D)']]);
  }
  var h2 = sheet.getRange(2, CURRENT_MONTHLY_COL).getValue();
  var h2Form = String(sheet.getRange(2, CURRENT_MONTHLY_COL).getFormula() || "");
  if (h2 === "" || h2 === null || h2Form.indexOf("EOMONTH") !== -1) {
    sheet.getRange(2, CURRENT_MONTHLY_COL).setFormula(
      '=SUMIFS(D:D,D:D,">0",B:B,">="&DATE(YEAR(TODAY()),MONTH(TODAY())-1,25),B:B,"<"&DATE(YEAR(TODAY()),MONTH(TODAY()),25))'
    );
  }

  var f3 = String(sheet.getRange(3, CATEGORY_COL).getValue()).trim();
  if (f3.toLowerCase() !== "saving") {
    sheet.getRange(3, CATEGORY_COL, 1, 2).setValues([["saving", '=SUM(D:D)']]);
  }
  var h3 = sheet.getRange(3, CURRENT_MONTHLY_COL).getValue();
  var h3Form = String(sheet.getRange(3, CURRENT_MONTHLY_COL).getFormula() || "");
  if (h3 === "" || h3 === null || h3Form.indexOf("EOMONTH") !== -1) {
    sheet.getRange(3, CURRENT_MONTHLY_COL).setFormula(
      '=SUMIFS(D:D,B:B,">="&DATE(YEAR(TODAY()),MONTH(TODAY())-1,25),B:B,"<"&DATE(YEAR(TODAY()),MONTH(TODAY()),25))'
    );
  }

  // Auto-populate default 14 categories into column F from F4 to F17 with formulas in G4:G17 and H4:H17 if F4 is empty
  var lastRow = sheet.getLastRow();
  if (lastRow < 4 || String(sheet.getRange(4, CATEGORY_COL).getValue()).trim() === "") {
    var rows = DEFAULT_CATEGORIES.map(function(c, idx) {
      var r = idx + 4;
      return [
        c,
        '=SUMIF(E:E,"*"&F' + r + '&"*",D:D)',
        '=SUMIFS(D:D,E:E,"*"&F' + r + '&"*",B:B,">="&DATE(YEAR(TODAY()),MONTH(TODAY())-1,25),B:B,"<"&DATE(YEAR(TODAY()),MONTH(TODAY()),25))'
      ];
    });
    sheet.getRange(4, CATEGORY_COL, rows.length, 3).setValues(rows);
  } else {
    // If rows already exist, migrate any old formulas using EOMONTH in column H
    for (var r = 4; r <= lastRow; r++) {
      var cell = sheet.getRange(r, CURRENT_MONTHLY_COL);
      var form = String(cell.getFormula() || "");
      if (form.indexOf("EOMONTH") !== -1) {
        cell.setFormula(
          '=SUMIFS(D:D,E:E,"*"&F' + r + '&"*",B:B,">="&DATE(YEAR(TODAY()),MONTH(TODAY())-1,25),B:B,"<"&DATE(YEAR(TODAY()),MONTH(TODAY()),25))'
        );
      }
    }
  }
}

function getMonthlySpending(sheet, body) {
  ensureCategoryHeaders(sheet);
  var h2 = Number(sheet.getRange(2, CURRENT_MONTHLY_COL).getValue()) || 0;
  var h3 = Number(sheet.getRange(3, CURRENT_MONTHLY_COL).getValue()) || 0;
  var income = h2;
  var remaining = h3;

  var targetCategory = String(body.category || "").trim().toLowerCase();
  var categoryAmount = 0;
  var categoryFound = false;

  var lastRow = sheet.getLastRow();
  var categories = [];
  if (lastRow >= 4) {
    var data = sheet.getRange(4, CATEGORY_COL, lastRow - 3, 3).getValues();
    for (var i = 0; i < data.length; i++) {
      var cat = String(data[i][0] || "").trim();
      if (!cat) continue;
      var catLower = cat.toLowerCase();
      var rawAmt = Number(data[i][2]) || 0; // Column H (index 2 in F..H)
      var absAmt = Math.abs(rawAmt);
      categories.push({
        category: cat,
        amount: absAmt,
        raw_amount: rawAmt
      });

      if (targetCategory && catLower === targetCategory) {
        categoryAmount = absAmt;
        categoryFound = true;
      }
    }
  }

  // Spent for this monthly cycle: income (H2) - remaining (H3)
  var spent = h2 - h3;

  return jsonResponse({
    ok: true,
    income: income,
    remaining: remaining,
    spent: spent,
    category: body.category || "",
    category_found: categoryFound,
    category_amount: categoryAmount,
    categories: categories
  });
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
