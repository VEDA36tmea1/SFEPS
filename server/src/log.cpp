#include "log.h"
#include "db_tls.h"

#include <cstring>
#include <iostream>
#include <vector>

namespace {
constexpr const char* kSystemLogInsertQuery = "INSERT INTO logs (event_type, message) VALUES (?, ?)";
constexpr const char* kLoginLogInsertQuery =
    "INSERT INTO login_logs (username, ip_address, status) VALUES (?, ?, ?)";
constexpr const char* kAnalyticsLogInsertQuery =
    "INSERT INTO analytics_logs (frame_time, object_type, created_at, estimated_age, photo_path, x, y, event) "
    "VALUES (?, ?, NOW(), ?, ?, ?, ?, ?)";
constexpr const char* kRecordingInsertQuery = "INSERT INTO recordings (filename) VALUES (?)";

bool prepare_stmt(MYSQL* conn, MYSQL_STMT*& stmt, const char* query, const char* name) {
    stmt = mysql_stmt_init(conn);
    if (stmt == nullptr) {
        std::cerr << "[log.cpp] " << "[DB Error] mysql_stmt_init() failed for " << name << std::endl;
        return false;
    }

    if (mysql_stmt_prepare(stmt, query, std::strlen(query)) != 0) {
        std::cerr << "[log.cpp] " << "[DB Error] prepare failed for " << name << ": "
                  << mysql_stmt_error(stmt) << std::endl;
        mysql_stmt_close(stmt);
        stmt = nullptr;
        return false;
    }
    return true;
}
} // namespace

DBLogger::DBLogger(const char* host_, const char* user_, const char* pass_, const char* db)
    : isRunning(false),
      conn(NULL),
      systemLogStmt(NULL),
      loginLogStmt(NULL),
      analyticsLogStmt(NULL),
      recordingStmt(NULL),
      host(host_ ? host_ : ""),
      user(user_ ? user_ : ""),
      pass(pass_ ? pass_ : ""),
      db_name(db ? db : "") {}

DBLogger::~DBLogger() {
    isRunning = false;
    cv.notify_one();
    if (workerThread.joinable()) workerThread.join();
    closeStatements();
    if (conn != NULL) {
        mysql_close(conn);
        std::cout << "[log.cpp] " << "[System] DB Connection Closed." << std::endl;
    }
}

bool DBLogger::prepareStatements() {
    if (conn == NULL) return false;
    if (!prepare_stmt(conn, systemLogStmt, kSystemLogInsertQuery, "system log insert")) return false;
    if (!prepare_stmt(conn, loginLogStmt, kLoginLogInsertQuery, "login log insert")) return false;
    if (!prepare_stmt(conn, analyticsLogStmt, kAnalyticsLogInsertQuery, "analytics log insert")) return false;
    if (!prepare_stmt(conn, recordingStmt, kRecordingInsertQuery, "recording insert")) return false;
    return true;
}

void DBLogger::closeStatements() {
    if (recordingStmt != NULL) {
        mysql_stmt_close(recordingStmt);
        recordingStmt = NULL;
    }
    if (analyticsLogStmt != NULL) {
        mysql_stmt_close(analyticsLogStmt);
        analyticsLogStmt = NULL;
    }
    if (loginLogStmt != NULL) {
        mysql_stmt_close(loginLogStmt);
        loginLogStmt = NULL;
    }
    if (systemLogStmt != NULL) {
        mysql_stmt_close(systemLogStmt);
        systemLogStmt = NULL;
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

    if (!prepareStatements()) {
        closeStatements();
        mysql_close(conn);
        conn = NULL;
        return false;
    }

    cleanupSizeQuery =
        "SELECT (data_length + index_length) / 1024 / 1024 FROM information_schema.tables WHERE table_schema = '" +
        db_name + "' AND table_name = 'analytics_logs'";

    isRunning = true;
    workerThread = std::thread(&DBLogger::processQueue, this);
    return true;
}

// 1. 일반 로그 큐에 넣기
void DBLogger::enqueue(const std::string& type, const std::string& message) {
    std::lock_guard<std::mutex> lock(queueMutex);
    logQueue.push(LogItem {SYSTEM_LOG, type, message, "", 0.0f, 0.0f, "", 0, "", false});
    cv.notify_one();
}

// 2. 로그인 로그 큐에 넣기
void DBLogger::enqueueLogin(const std::string& username, const std::string& ip, bool success) {
    std::lock_guard<std::mutex> lock(queueMutex);
    logQueue.push(LogItem {LOGIN_LOG, username, ip, "", 0.0f, 0.0f, "", 0, "", success});
    cv.notify_one();
}

// 3. ★ [수정됨] 분석 로그 큐에 넣기
// 인자가 x, y, event, age, photoPath로 변경됨
void DBLogger::enqueueAnalytics(const std::string& time,
                                const std::string& objType,
                                float x,
                                float y,
                                const std::string& event,
                                int age,
                                const std::string& photoPath) {
    std::lock_guard<std::mutex> lock(queueMutex);
    logQueue.push(LogItem {ANALYTICS_LOG, objType, "", time, x, y, event, age, photoPath, false});
    cv.notify_one();
}

// [신규] 녹화 파일 기록 큐에 넣기
void DBLogger::enqueueRecording(const std::string& filename) {
    std::lock_guard<std::mutex> lock(queueMutex);
    logQueue.push(LogItem {RECORDING_LOG, filename, "", "", 0.0f, 0.0f, "", 0, "", false});
    cv.notify_one();
}

// [신규] DB 청소 요청을 큐에 넣기
void DBLogger::requestDbCleanup() {
    std::lock_guard<std::mutex> lock(queueMutex);
    logQueue.push(LogItem {CLEANUP_DB_LOG, "", "", "", 0.0f, 0.0f, "", 0, "", false});
    cv.notify_one();
}

// 일꾼 스레드 (실제 DB 저장)
void DBLogger::processQueue() {
    auto execute_stmt = [&](MYSQL_STMT* stmt, MYSQL_BIND* bind, const char* stmt_name) -> bool {
        if (stmt == NULL) return false;
        if (mysql_stmt_reset(stmt) != 0) {
            std::cerr << "[log.cpp] " << "[DB Error] stmt reset failed (" << stmt_name
                      << "): " << mysql_stmt_error(stmt) << std::endl;
            return false;
        }
        if (mysql_stmt_bind_param(stmt, bind) != 0) {
            std::cerr << "[log.cpp] " << "[DB Error] stmt bind failed (" << stmt_name
                      << "): " << mysql_stmt_error(stmt) << std::endl;
            return false;
        }
        if (mysql_stmt_execute(stmt) != 0) {
            std::cerr << "[log.cpp] " << "[DB Error] stmt execute failed (" << stmt_name
                      << "): " << mysql_stmt_error(stmt) << std::endl;
            return false;
        }
        return true;
    };

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
            if (item.type == SYSTEM_LOG) {
                MYSQL_BIND params[2];
                std::memset(params, 0, sizeof(params));
                unsigned long type_len = static_cast<unsigned long>(item.str1.size());
                unsigned long msg_len = static_cast<unsigned long>(item.str2.size());

                params[0].buffer_type = MYSQL_TYPE_STRING;
                params[0].buffer = const_cast<char*>(item.str1.c_str());
                params[0].buffer_length = type_len;
                params[0].length = &type_len;

                params[1].buffer_type = MYSQL_TYPE_STRING;
                params[1].buffer = const_cast<char*>(item.str2.c_str());
                params[1].buffer_length = msg_len;
                params[1].length = &msg_len;

                execute_stmt(systemLogStmt, params, "logs");
            } else if (item.type == LOGIN_LOG) {
                MYSQL_BIND params[3];
                std::memset(params, 0, sizeof(params));
                const std::string status = (item.login_success) ? "SUCCESS" : "FAIL";

                unsigned long user_len = static_cast<unsigned long>(item.str1.size());
                unsigned long ip_len = static_cast<unsigned long>(item.str2.size());
                unsigned long status_len = static_cast<unsigned long>(status.size());

                params[0].buffer_type = MYSQL_TYPE_STRING;
                params[0].buffer = const_cast<char*>(item.str1.c_str());
                params[0].buffer_length = user_len;
                params[0].length = &user_len;

                params[1].buffer_type = MYSQL_TYPE_STRING;
                params[1].buffer = const_cast<char*>(item.str2.c_str());
                params[1].buffer_length = ip_len;
                params[1].length = &ip_len;

                params[2].buffer_type = MYSQL_TYPE_STRING;
                params[2].buffer = const_cast<char*>(status.c_str());
                params[2].buffer_length = status_len;
                params[2].length = &status_len;

                execute_stmt(loginLogStmt, params, "login_logs");
            } else if (item.type == ANALYTICS_LOG) {
                MYSQL_BIND params[7];
                std::memset(params, 0, sizeof(params));
                int age_param = item.age;
                double x_param = static_cast<double>(item.x);
                double y_param = static_cast<double>(item.y);

                unsigned long frame_time_len = static_cast<unsigned long>(item.time_str.size());
                unsigned long object_type_len = static_cast<unsigned long>(item.str1.size());
                unsigned long photo_len = static_cast<unsigned long>(item.photo_path.size());
                unsigned long event_len = static_cast<unsigned long>(item.event.size());

                params[0].buffer_type = MYSQL_TYPE_STRING;
                params[0].buffer = const_cast<char*>(item.time_str.c_str());
                params[0].buffer_length = frame_time_len;
                params[0].length = &frame_time_len;

                params[1].buffer_type = MYSQL_TYPE_STRING;
                params[1].buffer = const_cast<char*>(item.str1.c_str());
                params[1].buffer_length = object_type_len;
                params[1].length = &object_type_len;

                params[2].buffer_type = MYSQL_TYPE_LONG;
                params[2].buffer = &age_param;

                params[3].buffer_type = MYSQL_TYPE_STRING;
                params[3].buffer = const_cast<char*>(item.photo_path.c_str());
                params[3].buffer_length = photo_len;
                params[3].length = &photo_len;

                params[4].buffer_type = MYSQL_TYPE_DOUBLE;
                params[4].buffer = &x_param;

                params[5].buffer_type = MYSQL_TYPE_DOUBLE;
                params[5].buffer = &y_param;

                params[6].buffer_type = MYSQL_TYPE_STRING;
                params[6].buffer = const_cast<char*>(item.event.c_str());
                params[6].buffer_length = event_len;
                params[6].length = &event_len;

                execute_stmt(analyticsLogStmt, params, "analytics_logs");
            } else if (item.type == RECORDING_LOG) {
                MYSQL_BIND params[1];
                std::memset(params, 0, sizeof(params));
                unsigned long filename_len = static_cast<unsigned long>(item.str1.size());
                params[0].buffer_type = MYSQL_TYPE_STRING;
                params[0].buffer = const_cast<char*>(item.str1.c_str());
                params[0].buffer_length = filename_len;
                params[0].length = &filename_len;

                execute_stmt(recordingStmt, params, "recordings");
            } else if (item.type == CLEANUP_DB_LOG) {
                if (mysql_query(conn, cleanupSizeQuery.c_str()) == 0) {
                    MYSQL_RES* res = mysql_store_result(conn);
                    if (res) mysql_free_result(res);
                }
            }
        }
    }
}
