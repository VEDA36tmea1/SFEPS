#ifndef ANALYTICS_H
#define ANALYTICS_H

#include <string>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mysql/mysql.h>

class AnalyticsProcessor {
public:
    AnalyticsProcessor(const char* host,
                       const char* user,
                       const char* pass,
                       const char* db,
                       int cam_w = 3840,
                       int cam_h = 2160);
    ~AnalyticsProcessor();

    bool start();
    void stop();
    void publishRaw(const std::string& raw);

private:
    void workerLoop();
    void processLine(const std::string& line);
    bool prepareStatements();
    void closeStatements();
    bool insertAnalyticsRow(const std::string& frame_time,
                            const std::string& object_type,
                            int estimated_age,
                            int x,
                            int y,
                            const std::string& event_name,
                            const std::string& photo_path);

    std::string host;
    std::string user;
    std::string pass;
    std::string db;
    int cam_w;
    int cam_h;

    MYSQL* conn;
    MYSQL_STMT* analyticsInsertStmt;
    std::thread worker;
    std::queue<std::string> q;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> running;
    std::size_t max_lines_per_batch;
    std::size_t max_queue_size;
    std::size_t drop_log_interval;
    std::atomic<std::uint64_t> dropped_line_limit_count;
    std::atomic<std::uint64_t> dropped_queue_count;
};

#endif
