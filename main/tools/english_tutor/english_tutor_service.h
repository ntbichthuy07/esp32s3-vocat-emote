#ifndef ENGLISH_TUTOR_SERVICE_H
#define ENGLISH_TUTOR_SERVICE_H

#include <string>
#include <vector>

// A practice session opened by self.tutor.start_session: the learner's current level plus the
// topic picked for this session (least-recently-used within that level -- plain rotation, no
// weakness bias; see the MVP plan for what's deferred to v2).
struct EnglishTutorSession {
    int level = 5;
    std::string level_name;
    int sessions_total = 0;
    std::string topic;
    std::vector<std::string> prompts;
};

// One mistake previously logged via self.tutor.log_mistake, as returned by get_progress.
struct EnglishTutorMistake {
    std::string id;
    std::string created_date;
    std::string category;
    std::string original;
    std::string better;
    std::string explanation;
};

struct EnglishTutorProgress {
    int level = 5;
    std::string level_name;
    int sessions_total = 0;
    std::vector<EnglishTutorMistake> recent_mistakes;
};

// Talks to a Google Apps Script Web App backed by a Google Sheet that stores the learner's
// level, topic bank, and mistake log. Deploy apps_script.gs (in this directory) as a Web App
// and fill in tools/service_config.h (copy it from service_config.example.h) before using any
// method below. All timestamps are assigned by the Apps Script itself, not the device, so
// nothing here depends on the ESP32's clock being synced.
class EnglishTutorService {
public:
    // Reads the stored level and picks that level's least-recently-used topic. Returns true
    // and fills out_session on success; false and out_error otherwise.
    static bool StartSession(EnglishTutorSession& out_session, std::string& out_error);

    // Logs one grammar/meaning correction. `explanation` and `category` may be empty. Returns
    // true and fills out_id on success; false and out_error otherwise.
    static bool LogMistake(const std::string& original, const std::string& better,
                            const std::string& explanation, const std::string& category,
                            std::string& out_id, std::string& out_error);

    // Updates the stored level (1-10). Returns true and fills out_level_name on success; false
    // and out_error otherwise (e.g. level out of range).
    static bool SetLevel(int level, std::string& out_level_name, std::string& out_error);

    // Appends a completed session's summary and increments the session count. Returns true on
    // success; false and out_error otherwise.
    static bool EndSession(const std::string& summary, std::string& out_error);

    // Fetches the learner's current level and up to 5 most recently logged mistakes. Returns
    // true and fills out_progress on success; false and out_error otherwise.
    static bool GetProgress(EnglishTutorProgress& out_progress, std::string& out_error);
};

#endif  // ENGLISH_TUTOR_SERVICE_H
