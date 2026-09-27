#ifndef ENGLISH_TUTOR_SERVICE_H
#define ENGLISH_TUTOR_SERVICE_H

#include <string>
#include <vector>

// One note due for review today, as returned inline by StartSession or by GetDueReview.
struct EnglishTutorReviewItem {
    std::string id;
    std::string original;
    std::string better;
    std::string category;
    std::string bucket;  // "weakness" or "vocabulary"
};

// A practice session opened by self.tutor.start_session: the learner's current level, the
// topic picked for this session (weakness-biased when the learner has a recent recurring
// weakness, otherwise least-recently-used within that level), and up to a couple of notes due
// for review today.
struct EnglishTutorSession {
    int level = 5;
    std::string level_name;
    int sessions_total = 0;
    std::string topic;
    std::vector<std::string> prompts;
    std::vector<EnglishTutorReviewItem> due_review;
};

// One mistake or vocabulary note previously logged via self.tutor.log_note, as returned by
// get_progress's recent_mistakes/recent_vocabulary lists.
struct EnglishTutorNote {
    std::string id;
    std::string created_date;
    std::string category;
    std::string original;
    std::string better;
    std::string explanation;
};

struct EnglishTutorVocabStats {
    int in_review = 0;
    int mastered = 0;
};

struct EnglishTutorProgress {
    int level = 5;
    std::string level_name;
    int sessions_total = 0;
    std::string top_weakness;  // most frequent recent weakness category, "" if none yet
    std::vector<EnglishTutorNote> recent_mistakes;
    std::vector<EnglishTutorNote> recent_vocabulary;
    EnglishTutorVocabStats vocab_stats;
};

// Talks to a Google Apps Script Web App backed by a Google Sheet that stores the learner's
// level, topic bank, and mistake/vocabulary log. Deploy apps_script.gs (in this directory) as
// a Web App and fill in tools/service_config.h (copy it from service_config.example.h) before
// using any method below. All timestamps are assigned by the Apps Script itself, not the
// device, so nothing here depends on the ESP32's clock being synced.
class EnglishTutorService {
public:
    // Reads the stored level, picks a topic (weakness-biased, falling back to
    // least-recently-used), and previews up to 2 due review items. Returns true and fills
    // out_session on success; false and out_error otherwise.
    static bool StartSession(EnglishTutorSession& out_session, std::string& out_error);

    // Logs one correction (`bucket` "weakness") or native/idiomatic expression (`bucket`
    // "vocabulary"). `explanation` and `category` may be empty. Returns true and fills out_id
    // on success; false and out_error otherwise.
    static bool LogNote(const std::string& original, const std::string& better,
                         const std::string& explanation, const std::string& category,
                         const std::string& bucket, std::string& out_id, std::string& out_error);

    // Fetches up to `limit` notes due for review today (oldest-due first), optionally filtered
    // to `bucket` ("weakness"/"vocabulary"; empty means both). Returns true and fills out_items
    // on success; false and out_error otherwise.
    static bool GetDueReview(int limit, const std::string& bucket,
                              std::vector<EnglishTutorReviewItem>& out_items,
                              std::string& out_error);

    // Updates spaced-repetition scheduling for the note with the given id after quizzing the
    // learner on it. Returns true and fills out_next_review_date on success; false and
    // out_error otherwise (e.g. no such id).
    static bool RecordReview(const std::string& id, bool correct, std::string& out_next_review_date,
                              std::string& out_error);

    // Updates the stored level (1-10). Returns true and fills out_level_name on success; false
    // and out_error otherwise (e.g. level out of range).
    static bool SetLevel(int level, std::string& out_level_name, std::string& out_error);

    // Appends a completed session's summary, increments the session count, and refreshes the
    // cached top weakness used to bias future topic selection. Returns true on success; false
    // and out_error otherwise.
    static bool EndSession(const std::string& summary, std::string& out_error);

    // Fetches the learner's current level, cached top weakness, vocabulary stats, and up to 5
    // most recently logged mistakes/vocabulary notes. Returns true and fills out_progress on
    // success; false and out_error otherwise.
    static bool GetProgress(EnglishTutorProgress& out_progress, std::string& out_error);
};

#endif  // ENGLISH_TUTOR_SERVICE_H
