// Google Apps Script Web App backing the ESP32 self.tutor.* MCP tools (English Tutor with a
// dedicated Vocabulary sheet).
//
// Setup (fresh install):
//   1. Create a Google Sheet with five tabs:
//      - "Sessions": header row Timestamp | Summary
//      - "Topics": header row ID | Topic | Prompts | GrammarFocus | LastUsedDate -- seed a
//        handful of rows. Prompts is one cell holding one or more example opening questions
//        separated by " | ". GrammarFocus is an optional free-text tag (e.g. "past_tense")
//        naming the grammar point that topic naturally exercises -- leave it blank if none.
//        Leave LastUsedDate blank when seeding. topics_seed.csv (in this same directory) has
//        ready-made rows -- File > Import > Upload in this tab, "Insert new sheet(s)" off,
//        "Replace current sheet", or paste its rows in by hand.
//      - "Mistakes": header row ID | CreatedDate | Category | Original | Better | Explanation
//        | Bucket | Interval | EaseFactor | NextReviewDate | Reps -- grammar/meaning
//        corrections only (Bucket is always "weakness" now; vocabulary lives in its own sheet,
//        see below).
//      - "Vocabulary": header row ID | CreatedDate | Word | Meaning | Examples | Original |
//        Better | Category | Interval | EaseFactor | NextReviewDate | Reps | LastTaughtDate.
//        Two kinds of rows share this sheet:
//          * curated rows, seeded ahead of time like Topics: Word/Meaning/Examples filled in
//            (Examples is one cell, sentences separated by " | "), Original/Better left blank.
//            vocab_seed.csv has ready-made rows.
//          * logged rows, written automatically by self.tutor.log_note(bucket="vocabulary",
//            ...) when the tutor offers a native/idiomatic phrasing mid-conversation:
//            Original/Better/Category filled in, Word/Meaning/Examples left blank.
//        Both kinds flow through the same spaced-repetition columns once first surfaced.
//      - "Profile": header row SessionsCount | Weak, with an empty data row (row 2) below it
//        -- filled in automatically on first use.
//   2. Extensions > Apps Script, delete the sample code, paste this whole file in as Code.gs.
//   3. Change SHARED_SECRET below to a random string of your own.
//   4. Deploy > New deployment > type "Web app". Execute as: Me. Who has access: Anyone.
//   5. Copy tools/service_config.example.h to tools/service_config.h (gitignored), then put
//      the resulting URL (ends in /exec) into kTutorApiUrl there, and the same SHARED_SECRET
//      into kTutorApiSecret.
//   6. Any time you edit this file, go to Deploy > Manage deployments > pencil icon > Version:
//      "New version" > Deploy -- saving in the editor alone does not update the live /exec URL.
//
// No more level system: the 1-10 level scale (self.tutor.set_level, level/level_name in
// responses) has been removed entirely -- topics and vocabulary are picked from the whole bank
// rather than one level's worth, and the Level columns have been deleted from
// "Topics"/"Vocabulary"/"Profile" (every column below reflects that -- if you're upgrading an
// older sheet that still has a Level column, delete it in each of those three tabs first so the
// positions below line up).
//
// Upgrading a Sheet that's already in use: add a new "Vocabulary" tab with the columns above
// (seed it from vocab_seed.csv), and move any existing Bucket="vocabulary" rows out of
// "Mistakes" into "Vocabulary"'s Original/Better/Category columns by hand (there should be
// very few, since vocabulary tracking only shipped recently). Everything else keeps working
// under safe defaults with no other backfill needed. Then redeploy this file as a new version.
//
// All timestamps are the Apps Script project's own clock (Project Settings > Time zone) -- the
// device never sends a date, so this doesn't depend on the ESP32's clock being synced.

var SHARED_SECRET = "CHANGE_ME";
var SESSIONS_SHEET = "Sessions";
var TOPICS_SHEET = "Topics";
var MISTAKES_SHEET = "Mistakes";
var VOCAB_SHEET = "Vocabulary";
var PROFILE_SHEET = "Profile";
var RECENT_LIMIT = 5;
var DUE_REVIEW_DEFAULT_LIMIT = 5;
var DUE_REVIEW_MAX_LIMIT = 10;
var START_SESSION_DUE_REVIEW_PREVIEW = 2;
var WEAK_SCAN_LIMIT = 15;
var MASTERY_REPS = 3;
var DEFAULT_EASE_FACTOR = 2.5;

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
      case "get_vocab_word":
        return getVocabWord(body);
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

function splitPipeList(value) {
  return String(value || "").split("|").map(function(s) { return s.trim(); })
      .filter(function(s) { return s.length > 0; });
}

// --- Profile -------------------------------------------------------------

// Returns {sessionsCount, weak}. `weak` is the cached top weakness Category, refreshed by
// refreshProfileWeak() -- may be "" if there's no weakness data yet.
function getProfile() {
  var row = sheetByName(PROFILE_SHEET).getRange(2, 1, 1, 2).getValues()[0];
  return {sessionsCount: row[0] || 0, weak: row[1] || ""};
}

function setProfileWeak(weak) {
  sheetByName(PROFILE_SHEET).getRange(2, 2).setValue(weak || "");
}

// Reads-then-writes the sessions count; fine for a single personal user with no concurrent
// requests, which is the only case this needs to support.
function incrementProfileSessionsCount() {
  var profile = getProfile();
  var next = profile.sessionsCount + 1;
  sheetByName(PROFILE_SHEET).getRange(2, 1).setValue(next);
  return next;
}

// Recomputes Profile.Weak from the last WEAK_SCAN_LIMIT rows in Mistakes, by Category
// frequency. Called once per end_session, not on every read.
function refreshProfileWeak() {
  var sheet = sheetByName(MISTAKES_SHEET);
  var data = sheet.getDataRange().getValues();  // row 0 is the header

  var counts = {};
  var start = Math.max(1, data.length - WEAK_SCAN_LIMIT);
  for (var i = data.length - 1; i >= start; i--) {
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
  // plain least-recently-used if there's no match (or no weakness data yet).
  var matchIndex = -1, matchLastUsed = null;
  var fallbackIndex = -1, fallbackLastUsed = null;
  for (var i = 1; i < data.length; i++) {
    var grammarFocus = data[i][3] || "";
    var lastUsed = data[i][4] ? new Date(data[i][4]).getTime() : 0;

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
    return jsonResponse({ok: false, error: "no topics seeded"});
  }

  var chosen = data[bestRowIndex];
  topicsSheet.getRange(bestRowIndex + 1, 5).setValue(new Date());  // col 5 = E (LastUsedDate)

  return jsonResponse({
    ok: true,
    sessions_total: profile.sessionsCount,
    topic: chosen[1],
    prompts: splitPipeList(chosen[2]),
    due_review: getDueReviewItems(START_SESSION_DUE_REVIEW_PREVIEW, "")
  });
}

// --- log_note ----------------------------------------------------------

function logNote(body) {
  if (body.bucket === "vocabulary") {
    return logVocabularyNote(body);
  }
  return logMistakeNote(body);
}

function logMistakeNote(body) {
  var sheet = sheetByName(MISTAKES_SHEET);
  var id = "M" + (sheet.getLastRow() + 1);

  var nextReviewDate = new Date();
  nextReviewDate.setDate(nextReviewDate.getDate() + 1);  // first review: tomorrow

  sheet.appendRow([id, new Date(), body.category || "", body.original || "", body.better || "",
                    body.explanation || "", "weakness", 1, DEFAULT_EASE_FACTOR, nextReviewDate,
                    0]);
  return jsonResponse({ok: true, id: id});
}

function logVocabularyNote(body) {
  var sheet = sheetByName(VOCAB_SHEET);
  var id = "V" + (sheet.getLastRow() + 1);

  var nextReviewDate = new Date();
  nextReviewDate.setDate(nextReviewDate.getDate() + 1);  // first review: tomorrow

  // ID | CreatedDate | Word | Meaning | Examples | Original | Better | Category | Interval |
  // EaseFactor | NextReviewDate | Reps | LastTaughtDate
  sheet.appendRow([id, new Date(), "", "", "", body.original || "", body.better || "",
                    body.category || "", 1, DEFAULT_EASE_FACTOR, nextReviewDate, 0, ""]);
  return jsonResponse({ok: true, id: id});
}

// --- get_due_review / record_review -----------------------------------

function collectDueFromMistakes(today) {
  var data = sheetByName(MISTAKES_SHEET).getDataRange().getValues();
  var due = [];
  for (var i = 1; i < data.length; i++) {
    var row = data[i];
    var nextReviewMs = row[9] ? startOfDay(new Date(row[9])).getTime() : 0;
    if (nextReviewMs > today) continue;
    due.push({sortKey: nextReviewMs, id: row[0], original: row[3], better: row[4],
              category: row[2], bucket: "weakness"});
  }
  return due;
}

function collectDueFromVocabulary(today) {
  var data = sheetByName(VOCAB_SHEET).getDataRange().getValues();
  var due = [];
  for (var i = 1; i < data.length; i++) {
    var row = data[i];
    if (!row[10]) continue;  // never taught/logged yet -- not in the review cycle
    var nextReviewMs = startOfDay(new Date(row[10])).getTime();
    if (nextReviewMs > today) continue;
    due.push({
      sortKey: nextReviewMs,
      id: row[0],
      original: row[2] || row[5],  // Word, else Original
      better: row[3] || row[6],    // Meaning, else Better
      category: row[7] || "",
      bucket: "vocabulary"
    });
  }
  return due;
}

// Shared by start_session's preview and the standalone get_due_review action. Scans Mistakes
// and/or Vocabulary depending on bucketFilter ("weakness"/"vocabulary"/"" for both).
function getDueReviewItems(limit, bucketFilter) {
  var today = startOfDay(new Date()).getTime();
  var due = [];
  if (bucketFilter !== "vocabulary") due = due.concat(collectDueFromMistakes(today));
  if (bucketFilter !== "weakness") due = due.concat(collectDueFromVocabulary(today));
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
// spaced-repetition scheduling used by e.g. Anki. Looks up the id in Mistakes or Vocabulary
// based on its "M"/"V" prefix.
function recordReview(body) {
  var id = String(body.id || "");
  if (id.charAt(0) === "V") {
    return recordReviewInSheet(VOCAB_SHEET, id, !!body.correct, 8, 9, 10, 11);
  }
  return recordReviewInSheet(MISTAKES_SHEET, id, !!body.correct, 7, 8, 9, 10);
}

// interval/ease/nextReview/reps Col are 0-indexed column positions of those four fields, which
// must be contiguous and in that order (true for both Mistakes and Vocabulary).
function recordReviewInSheet(sheetName, id, correct, intervalCol, easeCol, nextReviewCol,
                              repsCol) {
  var sheet = sheetByName(sheetName);
  var data = sheet.getDataRange().getValues();

  var rowIndex = -1;
  for (var i = 1; i < data.length; i++) {
    if (String(data[i][0]) === id) {
      rowIndex = i;
      break;
    }
  }
  if (rowIndex === -1) {
    return jsonResponse({ok: false, error: "note id not found: " + id});
  }

  var row = data[rowIndex];
  var interval = Number(row[intervalCol]) || 1;
  var ease = Number(row[easeCol]) || DEFAULT_EASE_FACTOR;
  var reps = Number(row[repsCol]) || 0;

  if (correct) {
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
  sheet.getRange(rowIndex + 1, intervalCol + 1, 1, 4)
      .setValues([[interval, ease, nextReviewDate, reps]]);

  return jsonResponse({
    ok: true,
    next_review_date:
        Utilities.formatDate(nextReviewDate, Session.getScriptTimeZone(), "yyyy-MM-dd")
  });
}

// --- get_vocab_word ------------------------------------------------------

function getVocabWord(body) {
  var sheet = sheetByName(VOCAB_SHEET);
  var data = sheet.getDataRange().getValues();  // row 0 is the header

  var bestIndex = -1, bestLastTaught = null;
  for (var i = 1; i < data.length; i++) {
    var row = data[i];
    if (!row[2]) continue;  // only curated (Word-bearing) rows can be actively taught
    var lastTaught = row[12] ? new Date(row[12]).getTime() : 0;
    if (bestIndex === -1 || lastTaught < bestLastTaught) {
      bestIndex = i;
      bestLastTaught = lastTaught;
    }
  }
  if (bestIndex === -1) {
    return jsonResponse({ok: false, error: "no vocabulary seeded"});
  }

  var row = data[bestIndex];
  var sheetRow = bestIndex + 1;  // 1-indexed sheet rows
  sheet.getRange(sheetRow, 13).setValue(new Date());  // col 13 = M (LastTaughtDate)

  // First time this word is surfaced: start its spaced-repetition schedule too, so it joins
  // get_due_review/record_review going forward without a separate "start reviewing" step.
  if (!row[10]) {
    var nextReviewDate = new Date();
    nextReviewDate.setDate(nextReviewDate.getDate() + 1);
    sheet.getRange(sheetRow, 9, 1, 4).setValues([[1, DEFAULT_EASE_FACTOR, nextReviewDate, 0]]);
  }

  return jsonResponse({ok: true, word: row[2], meaning: row[3], examples: splitPipeList(row[4])});
}

// --- end_session -------------------------------------------------------

function endSession(body) {
  sheetByName(SESSIONS_SHEET).appendRow([new Date(), body.summary || ""]);
  incrementProfileSessionsCount();
  refreshProfileWeak();
  return jsonResponse({ok: true});
}

// --- get_progress --------------------------------------------------------

function collectRecentMistakes(limit) {
  var data = sheetByName(MISTAKES_SHEET).getDataRange().getValues();
  var result = [];
  for (var i = data.length - 1; i >= 1 && result.length < limit; i--) {
    var row = data[i];
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

function collectRecentVocabulary(limit) {
  var data = sheetByName(VOCAB_SHEET).getDataRange().getValues();
  var result = [];
  for (var i = data.length - 1; i >= 1 && result.length < limit; i--) {
    var row = data[i];
    result.push({
      id: row[0],
      created_date: row[1] ? new Date(row[1]).toISOString() : "",
      category: row[7] || "",
      original: row[2] || row[5],  // Word, else Original
      better: row[3] || row[6],    // Meaning, else Better
      explanation: ""
    });
  }
  return result;
}

function getProgress(body) {
  var profile = getProfile();
  var vocabData = sheetByName(VOCAB_SHEET).getDataRange().getValues();

  var vocabInReview = 0, vocabMastered = 0;
  for (var i = 1; i < vocabData.length; i++) {
    if (!vocabData[i][10]) continue;  // not in the review cycle yet
    var reps = Number(vocabData[i][11]) || 0;
    if (reps >= MASTERY_REPS) vocabMastered++;
    else vocabInReview++;
  }

  return jsonResponse({
    ok: true,
    sessions_total: profile.sessionsCount,
    top_weakness: profile.weak,
    recent_mistakes: collectRecentMistakes(RECENT_LIMIT),
    recent_vocabulary: collectRecentVocabulary(RECENT_LIMIT),
    vocab_stats: {in_review: vocabInReview, mastered: vocabMastered}
  });
}
