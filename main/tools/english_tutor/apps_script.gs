// Google Apps Script Web App backing the ESP32 self.tutor.* MCP tools (English Tutor V2).
//
// Setup (fresh install):
//   1. Create a Google Sheet with four tabs:
//      - "Sessions": header row Timestamp | Summary
//      - "Topics": header row ID | Level | Topic | Prompts | GrammarFocus | LastUsedDate --
//        seed a handful of rows per level (1-10). Prompts is one cell holding one or more
//        example opening questions separated by " | ". GrammarFocus is an optional free-text
//        tag (e.g. "past_tense") naming the grammar point that topic naturally exercises --
//        leave it blank if none. Leave LastUsedDate blank when seeding. topics_seed.csv (in
//        this same directory) has ready-made rows -- File > Import > Upload in this tab,
//        "Insert new sheet(s)" off, "Replace current sheet", or paste its rows in by hand.
//      - "Mistakes": header row ID | CreatedDate | Category | Original | Better | Explanation
//        | Bucket | Interval | EaseFactor | NextReviewDate | Reps. Despite the name, this
//        holds both corrected mistakes (Bucket "weakness") and native/idiomatic expressions
//        (Bucket "vocabulary") -- see self.tutor.log_note.
//      - "Profile": header row Level | SessionsCount | Weak, with an empty data row (row 2)
//        below it -- it's filled in automatically on first use, defaulting to Level 5.
//   2. Extensions > Apps Script, delete the sample code, paste this whole file in as Code.gs.
//   3. Change SHARED_SECRET below to a random string of your own.
//   4. Deploy > New deployment > type "Web app". Execute as: Me. Who has access: Anyone.
//   5. Copy tools/service_config.example.h to tools/service_config.h (gitignored), then put
//      the resulting URL (ends in /exec) into kTutorApiUrl there, and the same SHARED_SECRET
//      into kTutorApiSecret.
//   6. Any time you edit this file, go to Deploy > Manage deployments > pencil icon > Version:
//      "New version" > Deploy -- saving in the editor alone does not update the live /exec URL.
//
// Upgrading from the MVP (v1) on a Sheet that's already in use: add the new columns listed
// above to "Mistakes"/"Profile" at the end (headers are for readability only, the script
// writes to fixed column positions either way). "Topics" is the one exception -- GrammarFocus
// (E) sits *before* the existing LastUsedDate (now F), so right-click column E's header and
// "Insert 1 column left" first, which shifts the existing LastUsedDate data over to F for you;
// don't just append GrammarFocus after the existing LastUsedDate column. Then redeploy this
// file as a new version -- existing rows are read with safe defaults elsewhere (blank Bucket =
// "weakness", blank NextReviewDate = due now), nothing else needs to be backfilled.
//
// All timestamps are the Apps Script project's own clock (Project Settings > Time zone) -- the
// device never sends a date, so this doesn't depend on the ESP32's clock being synced.

var SHARED_SECRET = "CHANGE_ME";
var SESSIONS_SHEET = "Sessions";
var TOPICS_SHEET = "Topics";
var MISTAKES_SHEET = "Mistakes";
var PROFILE_SHEET = "Profile";
var DEFAULT_LEVEL = 5;
var RECENT_LIMIT = 5;
var DUE_REVIEW_DEFAULT_LIMIT = 5;
var DUE_REVIEW_MAX_LIMIT = 10;
var START_SESSION_DUE_REVIEW_PREVIEW = 2;
var WEAK_SCAN_LIMIT = 15;
var MASTERY_REPS = 3;
var DEFAULT_EASE_FACTOR = 2.5;
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
      case "log_note":
        return logNote(body);
      case "get_due_review":
        return getDueReview(body);
      case "record_review":
        return recordReview(body);
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

function startOfDay(date) {
  var d = new Date(date.getTime());
  d.setHours(0, 0, 0, 0);
  return d;
}

// --- Profile -------------------------------------------------------------

// Returns {level, sessionsCount, weak}, initializing row 2 with defaults the first time it's
// read. `weak` is the cached top weakness Category, refreshed by refreshProfileWeak() -- may
// be "" if there's no weakness data yet.
function getProfile() {
  var sheet = sheetByName(PROFILE_SHEET);
  var row = sheet.getRange(2, 1, 1, 3).getValues()[0];
  var level = row[0];
  var sessionsCount = row[1];
  var weak = row[2] || "";
  if (!level) {
    level = DEFAULT_LEVEL;
    sessionsCount = 0;
    sheet.getRange(2, 1, 1, 2).setValues([[level, sessionsCount]]);
  }
  return {level: level, sessionsCount: sessionsCount || 0, weak: weak};
}

function setProfileLevel(level) {
  sheetByName(PROFILE_SHEET).getRange(2, 1).setValue(level);
}

function setProfileWeak(weak) {
  sheetByName(PROFILE_SHEET).getRange(2, 3).setValue(weak || "");
}

// Reads-then-writes the sessions count; fine for a single personal user with no concurrent
// requests, which is the only case this needs to support.
function incrementProfileSessionsCount() {
  var profile = getProfile();
  var next = profile.sessionsCount + 1;
  sheetByName(PROFILE_SHEET).getRange(2, 2).setValue(next);
  return next;
}

// Recomputes Profile.Weak from the last WEAK_SCAN_LIMIT "weakness" rows in Mistakes, by
// Category frequency. Called once per end_session, not on every read.
function refreshProfileWeak() {
  var sheet = sheetByName(MISTAKES_SHEET);
  var data = sheet.getDataRange().getValues();  // row 0 is the header

  var counts = {};
  var start = Math.max(1, data.length - WEAK_SCAN_LIMIT);
  for (var i = data.length - 1; i >= start; i--) {
    var bucket = data[i][6] || "weakness";
    if (bucket !== "weakness") continue;
    var category = data[i][2];
    if (!category) continue;
    counts[category] = (counts[category] || 0) + 1;
  }

  var topCategory = "";
  var topCount = 0;
  for (var key in counts) {
    if (counts[key] > topCount) {
      topCategory = key;
      topCount = counts[key];
    }
  }
  setProfileWeak(topCategory);
}

// --- start_session ---------------------------------------------------------

function startSession(body) {
  var profile = getProfile();
  var topicsSheet = sheetByName(TOPICS_SHEET);
  var data = topicsSheet.getDataRange().getValues();  // row 0 is the header

  // Prefer a topic whose GrammarFocus matches the learner's current top weakness; fall back to
  // plain least-recently-used at this level if there's no match (or no weakness data yet).
  var matchIndex = -1, matchLastUsed = null;
  var fallbackIndex = -1, fallbackLastUsed = null;
  for (var i = 1; i < data.length; i++) {
    if (data[i][1] != profile.level) continue;
    var grammarFocus = data[i][4] || "";
    var lastUsed = data[i][5] ? new Date(data[i][5]).getTime() : 0;

    if (fallbackIndex === -1 || lastUsed < fallbackLastUsed) {
      fallbackIndex = i;
      fallbackLastUsed = lastUsed;
    }
    if (profile.weak && grammarFocus === profile.weak &&
        (matchIndex === -1 || lastUsed < matchLastUsed)) {
      matchIndex = i;
      matchLastUsed = lastUsed;
    }
  }

  var bestRowIndex = matchIndex !== -1 ? matchIndex : fallbackIndex;
  if (bestRowIndex === -1) {
    return jsonResponse({ok: false, error: "no topics seeded for level " + profile.level});
  }

  var chosen = data[bestRowIndex];
  topicsSheet.getRange(bestRowIndex + 1, 6).setValue(new Date());  // col 6 = F (LastUsedDate)

  var prompts = String(chosen[3]).split("|").map(function(p) { return p.trim(); });

  return jsonResponse({
    ok: true,
    level: profile.level,
    level_name: levelName(profile.level),
    sessions_total: profile.sessionsCount,
    topic: chosen[2],
    prompts: prompts,
    due_review: getDueReviewItems(START_SESSION_DUE_REVIEW_PREVIEW, "")
  });
}

// --- log_note / get_due_review / record_review -----------------------------

function logNote(body) {
  var sheet = sheetByName(MISTAKES_SHEET);
  var id = "M" + (sheet.getLastRow() + 1);
  var bucket = body.bucket === "vocabulary" ? "vocabulary" : "weakness";

  var nextReviewDate = new Date();
  nextReviewDate.setDate(nextReviewDate.getDate() + 1);  // first review: tomorrow

  sheet.appendRow([id, new Date(), body.category || "", body.original || "", body.better || "",
                    body.explanation || "", bucket, 1, DEFAULT_EASE_FACTOR, nextReviewDate, 0]);
  return jsonResponse({ok: true, id: id});
}

// Shared by start_session's preview and the standalone get_due_review action.
function getDueReviewItems(limit, bucketFilter) {
  var sheet = sheetByName(MISTAKES_SHEET);
  var data = sheet.getDataRange().getValues();  // row 0 is the header
  var today = startOfDay(new Date()).getTime();

  var due = [];
  for (var i = 1; i < data.length; i++) {
    var row = data[i];
    var bucket = row[6] || "weakness";
    if (bucketFilter && bucket !== bucketFilter) continue;

    var nextReviewRaw = row[9];
    var nextReviewMs = nextReviewRaw ? startOfDay(new Date(nextReviewRaw)).getTime() : 0;
    if (nextReviewMs > today) continue;  // not due yet

    due.push({
      sortKey: nextReviewMs,
      id: row[0],
      original: row[3],
      better: row[4],
      category: row[2],
      bucket: bucket
    });
  }
  due.sort(function(a, b) { return a.sortKey - b.sortKey; });
  return due.slice(0, limit).map(function(item) {
    return {id: item.id, original: item.original, better: item.better, category: item.category,
            bucket: item.bucket};
  });
}

function getDueReview(body) {
  var limit = Number(body.limit) || DUE_REVIEW_DEFAULT_LIMIT;
  limit = Math.min(Math.max(limit, 1), DUE_REVIEW_MAX_LIMIT);
  return jsonResponse({ok: true, items: getDueReviewItems(limit, body.bucket || "")});
}

// Simplified SM-2: correct answers grow the interval (1 -> 6 -> interval*ease, ease nudged up);
// an incorrect answer resets the interval to 1 rep and nudges ease down, matching the standard
// spaced-repetition scheduling used by e.g. Anki.
function recordReview(body) {
  var sheet = sheetByName(MISTAKES_SHEET);
  var data = sheet.getDataRange().getValues();

  var rowIndex = -1;
  for (var i = 1; i < data.length; i++) {
    if (String(data[i][0]) === String(body.id)) {
      rowIndex = i;
      break;
    }
  }
  if (rowIndex === -1) {
    return jsonResponse({ok: false, error: "note id not found: " + body.id});
  }

  var row = data[rowIndex];
  var interval = Number(row[7]) || 1;
  var ease = Number(row[8]) || DEFAULT_EASE_FACTOR;
  var reps = Number(row[10]) || 0;

  if (body.correct) {
    reps += 1;
    if (reps === 1) interval = 1;
    else if (reps === 2) interval = 6;
    else interval = Math.round(interval * ease);
    ease = Math.min(2.5, ease + 0.1);
  } else {
    reps = 0;
    interval = 1;
    ease = Math.max(1.3, ease - 0.2);
  }

  var nextReviewDate = new Date();
  nextReviewDate.setDate(nextReviewDate.getDate() + interval);
  sheet.getRange(rowIndex + 1, 8, 1, 4).setValues([[interval, ease, nextReviewDate, reps]]);

  return jsonResponse({
    ok: true,
    next_review_date: Utilities.formatDate(nextReviewDate, Session.getScriptTimeZone(), "yyyy-MM-dd")
  });
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
  refreshProfileWeak();
  return jsonResponse({ok: true});
}

// --- get_progress --------------------------------------------------------

function collectRecent(data, bucketFilter, limit) {
  var result = [];
  for (var i = data.length - 1; i >= 1 && result.length < limit; i--) {
    var row = data[i];
    var bucket = row[6] || "weakness";
    if (bucket !== bucketFilter) continue;
    result.push({
      id: row[0],
      created_date: row[1] ? new Date(row[1]).toISOString() : "",
      category: row[2],
      original: row[3],
      better: row[4],
      explanation: row[5]
    });
  }
  return result;
}

function getProgress(body) {
  var profile = getProfile();
  var sheet = sheetByName(MISTAKES_SHEET);
  var data = sheet.getDataRange().getValues();  // row 0 is the header

  var vocabInReview = 0, vocabMastered = 0;
  for (var i = 1; i < data.length; i++) {
    var bucket = data[i][6] || "weakness";
    if (bucket !== "vocabulary") continue;
    var reps = Number(data[i][10]) || 0;
    if (reps >= MASTERY_REPS) vocabMastered++;
    else vocabInReview++;
  }

  return jsonResponse({
    ok: true,
    level: profile.level,
    level_name: levelName(profile.level),
    sessions_total: profile.sessionsCount,
    top_weakness: profile.weak,
    recent_mistakes: collectRecent(data, "weakness", RECENT_LIMIT),
    recent_vocabulary: collectRecent(data, "vocabulary", RECENT_LIMIT),
    vocab_stats: {in_review: vocabInReview, mastered: vocabMastered}
  });
}
