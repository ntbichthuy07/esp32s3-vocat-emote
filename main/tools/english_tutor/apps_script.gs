// Google Apps Script Web App backing the ESP32 self.tutor.* MCP tools (English Tutor MVP).
//
// Setup:
//   1. Create a Google Sheet with four tabs:
//      - "Sessions": header row Timestamp | Summary
//      - "Topics": header row ID | Level | Topic | Prompts | LastUsedDate -- seed a handful of
//        rows per level (1-10). Prompts is one cell holding one or more example opening
//        questions separated by " | ", e.g. "What did you do this weekend? | Any plans for
//        next week?". Leave LastUsedDate blank when seeding. topics_seed.csv (in this same
//        directory) has 30 ready-made rows (3 per level) -- File > Import > Upload in this
//        tab, "Insert new sheet(s)" off, "Replace current sheet" or paste its rows in by hand.
//      - "Mistakes": header row ID | CreatedDate | Category | Original | Better | Explanation
//      - "Profile": header row Level | SessionsCount, with an empty data row (row 2) below it
//        -- it's filled in automatically on first use, defaulting to Level 5.
//   2. Extensions > Apps Script, delete the sample code, paste this whole file in as Code.gs.
//   3. Change SHARED_SECRET below to a random string of your own.
//   4. Deploy > New deployment > type "Web app". Execute as: Me. Who has access: Anyone.
//   5. Copy tools/service_config.example.h to tools/service_config.h (gitignored), then put
//      the resulting URL (ends in /exec) into kTutorApiUrl there, and the same SHARED_SECRET
//      into kTutorApiSecret.
//   6. Any time you edit this file, go to Deploy > Manage deployments > pencil icon > Version:
//      "New version" > Deploy -- saving in the editor alone does not update the live /exec URL.
//
// All timestamps are the Apps Script project's own clock (Project Settings > Time zone) -- the
// device never sends a date, so this doesn't depend on the ESP32's clock being synced.
//
// This is the MVP: topic selection is plain least-recently-used rotation within the learner's
// current level, with no weakness-awareness, no spaced-repetition scheduling, and no scoring.
// See the project plan for what's deferred to v2/v3.

var SHARED_SECRET = "CHANGE_ME";
var SESSIONS_SHEET = "Sessions";
var TOPICS_SHEET = "Topics";
var MISTAKES_SHEET = "Mistakes";
var PROFILE_SHEET = "Profile";
var DEFAULT_LEVEL = 5;
var RECENT_MISTAKES_LIMIT = 5;
var LEVEL_NAMES = ["", "Beginner", "Elementary", "Pre-intermediate", "Intermediate",
    "Upper-intermediate", "Conversation", "Work", "Discussion", "Advanced", "Fluent"];

function doPost(e) {
  try {
    var body = JSON.parse(e.postData.contents);
    if (body.token !== SHARED_SECRET) {
      return jsonResponse({ok: false, error: "invalid token"});
    }

    switch (body.action) {
      case "start_session":
        return startSession(body);
      case "log_mistake":
        return logMistake(body);
      case "set_level":
        return setLevel(body);
      case "end_session":
        return endSession(body);
      case "get_progress":
        return getProgress(body);
      default:
        return jsonResponse({ok: false, error: "unknown action: " + body.action});
    }
  } catch (err) {
    return jsonResponse({ok: false, error: String(err)});
  }
}

function jsonResponse(obj) {
  return ContentService.createTextOutput(JSON.stringify(obj))
      .setMimeType(ContentService.MimeType.JSON);
}

function levelName(level) {
  return LEVEL_NAMES[level] || LEVEL_NAMES[DEFAULT_LEVEL];
}

function sheetByName(name) {
  var sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(name);
  if (!sheet) throw "sheet '" + name + "' not found";
  return sheet;
}

// --- Profile -------------------------------------------------------------

// Returns {level, sessionsCount}, initializing row 2 with defaults the first time it's read.
function getProfile() {
  var sheet = sheetByName(PROFILE_SHEET);
  var row = sheet.getRange(2, 1, 1, 2).getValues()[0];
  var level = row[0];
  var sessionsCount = row[1];
  if (!level) {
    level = DEFAULT_LEVEL;
    sessionsCount = 0;
    sheet.getRange(2, 1, 1, 2).setValues([[level, sessionsCount]]);
  }
  return {level: level, sessionsCount: sessionsCount || 0};
}

function setProfileLevel(level) {
  sheetByName(PROFILE_SHEET).getRange(2, 1).setValue(level);
}

// Reads-then-writes the sessions count; fine for a single personal user with no concurrent
// requests, which is the only case this MVP needs to support.
function incrementProfileSessionsCount() {
  var profile = getProfile();
  var next = profile.sessionsCount + 1;
  sheetByName(PROFILE_SHEET).getRange(2, 2).setValue(next);
  return next;
}

// --- start_session ---------------------------------------------------------

function startSession(body) {
  var profile = getProfile();
  var topicsSheet = sheetByName(TOPICS_SHEET);
  var data = topicsSheet.getDataRange().getValues();  // row 0 is the header

  var bestRowIndex = -1;
  var bestLastUsed = null;
  for (var i = 1; i < data.length; i++) {
    var rowLevel = data[i][1];
    if (rowLevel != profile.level) continue;
    var lastUsed = data[i][4] ? new Date(data[i][4]).getTime() : 0;
    if (bestRowIndex === -1 || lastUsed < bestLastUsed) {
      bestRowIndex = i;
      bestLastUsed = lastUsed;
    }
  }
  if (bestRowIndex === -1) {
    return jsonResponse({ok: false, error: "no topics seeded for level " + profile.level});
  }

  var chosen = data[bestRowIndex];
  topicsSheet.getRange(bestRowIndex + 1, 5).setValue(new Date());  // +1: 1-indexed sheet rows

  var prompts = String(chosen[3]).split("|").map(function(p) { return p.trim(); });

  return jsonResponse({
    ok: true,
    level: profile.level,
    level_name: levelName(profile.level),
    sessions_total: profile.sessionsCount,
    topic: chosen[2],
    prompts: prompts
  });
}

// --- log_mistake -----------------------------------------------------------

function logMistake(body) {
  var sheet = sheetByName(MISTAKES_SHEET);
  var id = "M" + (sheet.getLastRow() + 1);
  sheet.appendRow([id, new Date(), body.category || "", body.original || "", body.better || "",
                    body.explanation || ""]);
  return jsonResponse({ok: true, id: id});
}

// --- set_level ---------------------------------------------------------

function setLevel(body) {
  var level = Number(body.level);
  if (!level || level < 1 || level > 10) {
    return jsonResponse({ok: false, error: "level must be an integer 1-10"});
  }
  setProfileLevel(level);
  return jsonResponse({ok: true, level: level, level_name: levelName(level)});
}

// --- end_session -------------------------------------------------------

function endSession(body) {
  sheetByName(SESSIONS_SHEET).appendRow([new Date(), body.summary || ""]);
  incrementProfileSessionsCount();
  return jsonResponse({ok: true});
}

// --- get_progress --------------------------------------------------------

function getProgress(body) {
  var profile = getProfile();
  var mistakesSheet = sheetByName(MISTAKES_SHEET);
  var data = mistakesSheet.getDataRange().getValues();  // row 0 is the header

  var recentMistakes = [];
  var start = Math.max(1, data.length - RECENT_MISTAKES_LIMIT);
  for (var i = data.length - 1; i >= start; i--) {
    var row = data[i];
    recentMistakes.push({
      id: row[0],
      created_date: row[1] ? new Date(row[1]).toISOString() : "",
      category: row[2],
      original: row[3],
      better: row[4],
      explanation: row[5]
    });
  }

  return jsonResponse({
    ok: true,
    level: profile.level,
    level_name: levelName(profile.level),
    sessions_total: profile.sessionsCount,
    recent_mistakes: recentMistakes
  });
}
