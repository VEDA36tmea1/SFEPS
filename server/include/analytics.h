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
#include <unordered_set>

#include <mysql/mysql.h>

class AnalyticsProcessor {
public:
    struct FraudBBoxPayload {
        std::string object_id;
        std::string card_age_text;
        std::string age_group;
        float left = -1.0f;
        float top = -1.0f;
        float right = -1.0f;
        float bottom = -1.0f;
    };

    using FraudBBoxCallback = std::function<void(const FraudBBoxPayload&)>;

    AnalyticsProcessor(const char* host,
                       const char* user,
                       const char* pass,
                       const char* db,
                       int cam_w = 3840,
                       int cam_h = 2160);
    ~AnalyticsProcessor();

    bool start();
    void stop();

    // Called by recorder thread with a complete XML metadata document.
    void publishRaw(const std::string& raw);

    // Called by RFID monitor thread with RFID text value.
    void onRfidRead(const std::string& card_age_text);
    void setFraudBBoxCallback(FraudBBoxCallback callback);

private:
    struct PendingObject {
        std::string object_id;
        std::string card_age_text;
        std::string age_group;
        bool is_fraud = false;
        float bbox_left = -1.0f;
        float bbox_top = -1.0f;
        float bbox_right = -1.0f;
        float bbox_bottom = -1.0f;
        std::chrono::steady_clock::time_point created_at;
    };

    struct FraudRecord {
        std::string object_id;
        std::string card_age_text;
        std::string age_group;
        bool is_fraud = false;
    };

    void workerLoop();
    bool prepareStatements();
    void closeStatements();
    bool insertAnalyticsRow(const FraudRecord& record);
    void pruneExpiredPendingLocked(std::chrono::steady_clock::time_point now);

    std::string host;
    std::string user;
    std::string pass;
    std::string db;
    int cam_w;
    int cam_h;

    MYSQL* conn;
    MYSQL_STMT* analyticsInsertStmt;
    std::thread worker;

    std::deque<PendingObject> pending_queue;
    std::unordered_set<std::string> pending_object_ids;
    std::queue<FraudRecord> q;

    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> running;

    std::size_t max_lines_per_batch;
    std::size_t max_queue_size;
    std::size_t max_pending_size;
    std::size_t pending_ttl_seconds;
    std::size_t drop_log_interval;

    std::atomic<std::uint64_t> dropped_line_limit_count;
    std::atomic<std::uint64_t> dropped_queue_count;
    std::atomic<std::uint64_t> dropped_invalid_xml_count;
    std::atomic<std::uint64_t> dropped_pending_expired_count;
    std::atomic<std::uint64_t> dropped_pending_overflow_count;
    std::atomic<std::uint64_t> parsed_xml_ok_count;
    FraudBBoxCallback fraud_bbox_callback;
};

#endif
