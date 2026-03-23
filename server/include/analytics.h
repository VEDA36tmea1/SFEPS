#ifndef ANALYTICS_H
#define ANALYTICS_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <mysql/mysql.h>

#include "XMLParser.h"

class AnalyticsProcessor {
public:
    struct ObjectPositionSnapshot {
        std::string object_id;
        float left = -1.0f;
        float top = -1.0f;
        float right = -1.0f;
        float bottom = -1.0f;
        float x = -1.0f;
        float y = -1.0f;
        bool is_fraud = false;
        std::string tag_time;
        std::chrono::steady_clock::time_point updated_at;
    };

    struct TrackPosPayload {
        std::string object_id;
        float left = -1.0f;
        float top = -1.0f;
        float right = -1.0f;
        float bottom = -1.0f;
        float x = -1.0f;
        float y = -1.0f;
    };

    struct OutlineDecisionPayload {
        std::string object_id;
        std::string card_age_text;
        std::string age;
        bool is_fraud = false;
        std::string tag_time;
    };

    using TrackPosCallback = std::function<void(const TrackPosPayload&)>;
    using RfidPairedCallback = std::function<void(const std::string&, const std::string&)>;
    using OutlineDecisionCallback = std::function<void(const OutlineDecisionPayload&)>;

    AnalyticsProcessor(const char* host,
                       const char* user,
                       const char* pass,
                       const char* db);
    ~AnalyticsProcessor();

    bool start();
    void stop();

    // Called by recorder thread with a complete XML metadata document.
    void publishRaw(const std::string& raw);

    // Called by RFID monitor thread with RFID text value.
    void onRfidRead(const std::string& card_age_text);
    bool getObjectPositionSnapshot(const std::string& object_id,
                                   ObjectPositionSnapshot& out) const;
    void getAllObjectSnapshots(std::vector<ObjectPositionSnapshot>& out) const;
    void setTrackPosCallback(TrackPosCallback callback);
    void setRfidPairedCallback(RfidPairedCallback callback);
    void setOutlineDecisionCallback(OutlineDecisionCallback callback);

private:
    struct PendingObject {
        std::string object_id;
        std::string card_age_text;
        std::string age;
        std::string enter_tag_time;
        std::string outline_tag_time;
        bool is_fraud = false;
        float bbox_left = -1.0f;
        float bbox_top = -1.0f;
        float bbox_right = -1.0f;
        float bbox_bottom = -1.0f;
        float center_x = -1.0f;
        float center_y = -1.0f;
        std::chrono::steady_clock::time_point created_at;
    };

    struct FraudRecord {
        std::string object_id;
        std::string card_age_text;
        std::string age;
        bool is_fraud = false;
    };

    struct LatestObjectInfo {
        float x = -1.0f;
        float y = -1.0f;
        float left = -1.0f;
        float top = -1.0f;
        float right = -1.0f;
        float bottom = -1.0f;
        std::string tag_time;
        std::chrono::steady_clock::time_point updated_at;
    };

    void workerLoop();
    bool prepareStatements();
    void closeStatements();
    bool insertAnalyticsRow(const FraudRecord& record);
    void pruneExpiredPendingLocked(std::chrono::steady_clock::time_point now);
    void pruneExpiredStateLocked(std::chrono::steady_clock::time_point now);

    std::string host;
    std::string user;
    std::string pass;
    std::string db;

    MYSQL* conn;
    MYSQL_STMT* analyticsInsertStmt;
    std::thread worker;

    std::deque<PendingObject> pending_queue;
    std::unordered_set<std::string> pending_object_ids;
    std::unordered_map<std::string, PendingObject> matched_objects;
    std::unordered_map<std::string, LatestObjectInfo> latest_objects;
    std::unordered_map<std::string, bool> object_fraud_flags;
    std::queue<FraudRecord> q;

    mutable std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> running;

    std::size_t max_lines_per_batch;
    std::size_t max_queue_size;
    std::size_t max_pending_size;
    std::size_t pending_ttl_seconds;
    std::size_t drop_log_interval;
    std::string enter_rule_name;
    std::string outline_rule_name;

    std::atomic<std::uint64_t> dropped_line_limit_count;
    std::atomic<std::uint64_t> dropped_queue_count;
    std::atomic<std::uint64_t> dropped_pending_expired_count;
    std::atomic<std::uint64_t> dropped_pending_overflow_count;
    std::atomic<std::uint64_t> parsed_xml_ok_count;
    TrackPosCallback track_pos_callback;
    RfidPairedCallback rfid_paired_callback;
    OutlineDecisionCallback outline_decision_callback;
    XMLParser xml_parser;
};

#endif
