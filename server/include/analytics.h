#ifndef ANALYTICS_H
#define ANALYTICS_H

#include <string>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <mysql/mysql.h>

class AnalyticsProcessor {
public:
    AnalyticsProcessor(const char* host, const char* user, const char* pass, const char* db, int cam_w = 3840, int cam_h = 2160);
    ~AnalyticsProcessor();

    bool start();
    void stop();
    void publishRaw(const std::string& raw);

private:
    void workerLoop();
    void processLine(const std::string& line);

    const char* host; const char* user; const char* pass; const char* db;
    int cam_w; int cam_h;

    MYSQL* conn;
    std::thread worker;
    std::queue<std::string> q;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> running;
};

#endif
