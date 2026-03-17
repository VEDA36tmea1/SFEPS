#include "log.h"

#include <cstring>
#include <iostream>
#include <vector>

namespace {
constexpr const char* kLoginLogInsertQuery =
    "INSERT INTO login_logs (username, ip_address, status) VALUES (?, ?, ?)";
constexpr const char* kRecordingInsertQuery = "INSERT INTO recordings (filename) VALUES (?)";
constexpr const char* kAnalyticsRetentionDeleteQuery =
    "DELETE FROM analytics_logs WHERE created_at < (NOW() - INTERVAL 1 DAY)";
constexpr const char* kLoginLogRetentionDeleteQuery =
    "DELETE FROM login_logs WHERE created_at < (NOW() - INTERVAL 1 DAY)";
constexpr const char* kRecordingRetentionDeleteByCreatedAtQuery =
    "DELETE FROM recordings WHERE created_at < (NOW() - INTERVAL 1 DAY)";
constexpr const char* kRecordingRetentionDeleteByFilenameQuery =
    "DELETE FROM recordings "
    "WHERE SUBSTRING_INDEX(filename, '/', -1) REGEXP '^rec_[0-9]{8}_[0-9]{6}\\\\.mp4$' "
    "AND STR_TO_DATE(SUBSTRING(SUBSTRING_INDEX(filename, '/', -1), 5, 15), '%Y%m%d_%H%i%s') "
    "< (NOW() - INTERVAL 1 DAY)";

bool prepare_stmt(MYSQL* conn, MYSQL_STMT*& stmt, const char* query, const char* name) {
    stmt = mysql_stmt_init(conn);
    if (stmt == nullptr) {
        std::cerr << "[log.cpp] [DB Error] mysql_stmt_init() failed for " << name << std::endl;
        return false;
    }

    if (mysql_stmt_prepare(stmt, query, std::strlen(query)) != 0) {
        std::cerr << "[log.cpp] [DB Error] prepare failed for " << name << ": "
                  << mysql_stmt_error(stmt) << std::endl;
        mysql_stmt_close(stmt);
        stmt = nullptr;
        return false;
    }

    return true;
}
}  // namespace

DBLogger::DBLogger(const char* host_, const char* user_, const char* pass_, const char* db)
    : conn(nullptr),
      loginLogStmt(nullptr),
      recordingStmt(nullptr),
      host(host_ ? host_ : ""),
      user(user_ ? user_ : ""),
      pass(pass_ ? pass_ : ""),
      db_name(db ? db : ""),
      isRunning(false) {}

DBLogger::~DBLogger() {
    isRunning = false;
    cv.notify_all();
    if (workerThread.joinable()) {
        workerThread.join();
    }

    closeStatements();

    if (conn != nullptr) {
        mysql_close(conn);
        conn = nullptr;
        std::cout << "[log.cpp] [System] DB connection closed." << std::endl;
    }
}

bool DBLogger::prepareStatements() {
    if (conn == nullptr) return false;

    if (!prepare_stmt(conn, loginLogStmt, kLoginLogInsertQuery, "login_logs insert")) return false;
    if (!prepare_stmt(conn, recordingStmt, kRecordingInsertQuery, "recordings insert")) return false;
    return true;
}

void DBLogger::closeStatements() {
    if (recordingStmt != nullptr) {
        mysql_stmt_close(recordingStmt);
        recordingStmt = nullptr;
    }
    if (loginLogStmt != nullptr) {
        mysql_stmt_close(loginLogStmt);
        loginLogStmt = nullptr;
    }
}

bool DBLogger::connect() {
    conn = mysql_init(nullptr);
    if (conn == nullptr) {
        return false;
    }

    if (mysql_real_connect(conn, host.c_str(), user.c_str(), pass.c_str(), db_name.c_str(), 0, nullptr, 0) ==
        nullptr) {
        std::cerr << "[log.cpp] [DB Error] " << mysql_error(conn) << std::endl;
        mysql_close(conn);
        conn = nullptr;
        return false;
    }

    if (!prepareStatements()) {
        closeStatements();
        mysql_close(conn);
        conn = nullptr;
        return false;
    }

    isRunning = true;
    workerThread = std::thread(&DBLogger::processQueue, this);
    return true;
}

void DBLogger::enqueueLogin(const std::string& username, const std::string& ip, bool success) {
    if (!isRunning.load()) return;

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        logQueue.push(LogItem {LOGIN_LOG, username, ip, success});
    }
    cv.notify_one();
}

void DBLogger::enqueueRecording(const std::string& filename) {
    if (!isRunning.load()) return;

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        logQueue.push(LogItem {RECORDING_LOG, filename, "", false});
    }
    cv.notify_one();
}

void DBLogger::requestDbCleanup() {
    if (!isRunning.load()) return;

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        logQueue.push(LogItem {CLEANUP_DB_LOG, "", "", false});
    }
    cv.notify_one();
}

void DBLogger::processQueue() {
    auto execute_stmt = [&](MYSQL_STMT* stmt, MYSQL_BIND* bind, const char* stmt_name) -> bool {
        if (stmt == nullptr) return false;

        if (mysql_stmt_reset(stmt) != 0) {
            std::cerr << "[log.cpp] [DB Error] stmt reset failed (" << stmt_name
                      << "): " << mysql_stmt_error(stmt) << std::endl;
            return false;
        }

        if (mysql_stmt_bind_param(stmt, bind) != 0) {
            std::cerr << "[log.cpp] [DB Error] stmt bind failed (" << stmt_name
                      << "): " << mysql_stmt_error(stmt) << std::endl;
            return false;
        }

        if (mysql_stmt_execute(stmt) != 0) {
            std::cerr << "[log.cpp] [DB Error] stmt execute failed (" << stmt_name
                      << "): " << mysql_stmt_error(stmt) << std::endl;
            return false;
        }

        return true;
    };

    auto execute_delete = [&](const char* query, const char* label,
                              my_ulonglong* affected_rows) -> int {
        if (mysql_query(conn, query) != 0) {
            const int err = mysql_errno(conn);
            std::cerr << "[log.cpp] [DB Error] cleanup delete failed (" << label
                      << "): " << mysql_error(conn) << std::endl;
            return err;
        }
        if (affected_rows != nullptr) {
            *affected_rows = mysql_affected_rows(conn);
        }
        return 0;
    };

    while (true) {
        std::vector<LogItem> batch;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            cv.wait(lock, [this] { return !logQueue.empty() || !isRunning.load(); });
            if (!isRunning.load() && logQueue.empty()) {
                break;
            }

            batch.reserve(logQueue.size());
            while (!logQueue.empty()) {
                batch.push_back(std::move(logQueue.front()));
                logQueue.pop();
            }
        }

        if (conn == nullptr || batch.empty()) continue;

        bool cleanup_requested = false;
        for (auto& item : batch) {
            if (item.type == LOGIN_LOG) {
                MYSQL_BIND params[3];
                std::memset(params, 0, sizeof(params));

                const std::string status = item.login_success ? "SUCCESS" : "FAIL";
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
                cleanup_requested = true;
            }
        }

        if (!cleanup_requested) continue;

        my_ulonglong deleted_analytics = 0;
        if (execute_delete(kAnalyticsRetentionDeleteQuery, "analytics_logs(created_at)",
                           &deleted_analytics) == 0 &&
            deleted_analytics > 0) {
            std::cout << "[log.cpp] [Cleanup] deleted analytics_logs rows: "
                      << deleted_analytics << std::endl;
        }

        execute_delete(kLoginLogRetentionDeleteQuery, "login_logs(created_at)", nullptr);

        int recording_cleanup_err = execute_delete(
            kRecordingRetentionDeleteByCreatedAtQuery, "recordings(created_at)",
            nullptr);
        if (recording_cleanup_err == 1054) {
            execute_delete(kRecordingRetentionDeleteByFilenameQuery,
                           "recordings(filename timestamp fallback)", nullptr);
        } else if (recording_cleanup_err != 0) {
            std::cerr
                << "[log.cpp] [DB Error] recordings cleanup skipped due to non-recoverable error."
                << std::endl;
        }
    }
}
