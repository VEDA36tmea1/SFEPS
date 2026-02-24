#include "log.h"
#include "db_tls.h"
#include <iostream>
#include <sstream>
#include <iomanip>

namespace {
std::string escapeField(MYSQL* conn, const std::string& s) {
    std::string out;
    out.resize(s.size() * 2 + 1);
    unsigned long new_len = mysql_real_escape_string(conn, &out[0], s.c_str(), s.size());
    out.resize(new_len);
    return out;
}
} // namespace

DBLogger::DBLogger(const char* host_, const char* user_, const char* pass_, const char* db)
    : isRunning(false),
      conn(NULL),
      host(host_ ? host_ : ""),
      user(user_ ? user_ : ""),
      pass(pass_ ? pass_ : ""),
      db_name(db ? db : "") {
}

DBLogger::~DBLogger() {
    isRunning = false;
    cv.notify_one(); 
    if (workerThread.joinable()) workerThread.join();
    if (conn != NULL) {
        mysql_close(conn);
        std::cout << "[log.cpp] " << "[System] DB Connection Closed." << std::endl;
    }
}

bool DBLogger::connect() {
    conn = mysql_init(NULL);
    if (conn == NULL) return false;

    std::string tls_err;
    if (!configure_db_tls(conn, "log", tls_err)) {
        std::cerr << "[DB TLS Error] " << tls_err << std::endl;
        mysql_close(conn);
        conn = NULL;
        return false;
    }

    if (mysql_real_connect(conn, host.c_str(), user.c_str(), pass.c_str(), db_name.c_str(), 0, NULL, 0) == NULL) {
        std::cerr << "[DB Error] " << mysql_error(conn) << std::endl;
        mysql_close(conn);
        conn = NULL;
        return false;
    }

    cleanupSizeQuery = "SELECT (data_length + index_length) / 1024 / 1024 FROM information_schema.tables WHERE table_schema = '" +
        db_name + "' AND table_name = 'analytics_logs'";
    
    isRunning = true;
    workerThread = std::thread(&DBLogger::processQueue, this);
    return true;
}

// 1. 일반 로그 큐에 넣기
void DBLogger::enqueue(const std::string& type, const std::string& message) {
    std::lock_guard<std::mutex> lock(queueMutex);
    logQueue.push(LogItem{SYSTEM_LOG, type, message, "", 0.0f, 0.0f, "", 0, "", false});
    cv.notify_one();
}

// 2. 로그인 로그 큐에 넣기
void DBLogger::enqueueLogin(const std::string& username, const std::string& ip, bool success) {
    std::lock_guard<std::mutex> lock(queueMutex);
    logQueue.push(LogItem{LOGIN_LOG, username, ip, "", 0.0f, 0.0f, "", 0, "", success});
    cv.notify_one();
}

// 3. ★ [수정됨] 분석 로그 큐에 넣기
// 인자가 x, y, event, age, photoPath로 변경됨
void DBLogger::enqueueAnalytics(const std::string& time, const std::string& objType, 
                                float x, float y, const std::string& event, 
                                int age, const std::string& photoPath) {
    std::lock_guard<std::mutex> lock(queueMutex);
    logQueue.push(LogItem{ANALYTICS_LOG, objType, "", time, x, y, event, age, photoPath, false});
    cv.notify_one();
}

// [신규] 녹화 파일 기록 큐에 넣기
void DBLogger::enqueueRecording(const std::string& filename) {
    std::lock_guard<std::mutex> lock(queueMutex);
    logQueue.push(LogItem{RECORDING_LOG, filename, "", "", 0.0f, 0.0f, "", 0, "", false});
    cv.notify_one();
}

// [신규] DB 청소 요청을 큐에 넣기
void DBLogger::requestDbCleanup() {
    std::lock_guard<std::mutex> lock(queueMutex);
    logQueue.push(LogItem{CLEANUP_DB_LOG, "", "", "", 0.0f, 0.0f, "", 0, "", false});
    cv.notify_one();
}

// 일꾼 스레드 (실제 DB 저장)
void DBLogger::processQueue() {
    while (isRunning) {
        std::vector<LogItem> batch;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            cv.wait(lock, [this] { return !logQueue.empty() || !isRunning; });
            if (!isRunning && logQueue.empty()) break;
            batch.reserve(logQueue.size());
            while (!logQueue.empty()) {
                batch.push_back(std::move(logQueue.front()));
                logQueue.pop();
            }
        }

        if (!conn || batch.empty()) continue;

        for (auto& item : batch) {
            std::string query;

            if (item.type == SYSTEM_LOG) {
                query = "INSERT INTO logs (event_type, message) VALUES ('";
                query += escapeField(conn, item.str1);
                query += "', '";
                query += escapeField(conn, item.str2);
                query += "')";
            } else if (item.type == LOGIN_LOG) {
                std::string status = (item.login_success) ? "SUCCESS" : "FAIL";
                query = "INSERT INTO login_logs (username, ip_address, status) VALUES ('";
                query += escapeField(conn, item.str1);
                query += "', '";
                query += escapeField(conn, item.str2);
                query += "', '";
                query += status;
                query += "')";
            } else if (item.type == ANALYTICS_LOG) {
                std::string esc_obj = escapeField(conn, item.str1);
                std::string esc_photo = escapeField(conn, item.photo_path);
                std::string esc_event = escapeField(conn, item.event);
                std::string esc_time = escapeField(conn, item.time_str);

                std::ostringstream oss;
                oss << "INSERT INTO analytics_logs (frame_time, object_type, created_at, estimated_age, photo_path, x, y, event) VALUES ('";
                oss << esc_time << "', '" << esc_obj << "', NOW(), '" << item.age << "', '" << esc_photo << "', ";
                oss << std::fixed << std::setprecision(2) << item.x << ", " << item.y << ", '" << esc_event << "')";
                query = oss.str();
            } else if (item.type == RECORDING_LOG) {
                query = "INSERT INTO recordings (filename) VALUES ('";
                query += escapeField(conn, item.str1);
                query += "')";
            } else if (item.type == CLEANUP_DB_LOG) {
                if (mysql_query(conn, cleanupSizeQuery.c_str()) == 0) {
                    MYSQL_RES* res = mysql_store_result(conn);
                    if (res) mysql_free_result(res);
                }
                continue;
            }

            if (!query.empty()) {
                if (mysql_query(conn, query.c_str())) {
                    std::cerr << "[DB Error] " << mysql_error(conn) << std::endl;
                    std::cout << "[log.cpp] " << "Query: " << query << std::endl;
                }
            }
        }
    }
}
