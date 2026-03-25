#include "log.h"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

#include "video_catalog_events.h"

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
        std::cerr << "[log.cpp] [DB Error] mysql_stmt_init() 실패: " << name << std::endl;
        return false;
    }

    if (mysql_stmt_prepare(stmt, query, std::strlen(query)) != 0) {
        std::cerr << "[log.cpp] [DB Error] prepare 실패: " << name << ": "
                  << mysql_stmt_error(stmt) << std::endl;
        mysql_stmt_close(stmt);
        stmt = nullptr;
        return false;
    }

    return true;
}

bool execute_stmt(MYSQL_STMT* stmt, MYSQL_BIND* bind, const char* stmt_name) {
    if (stmt == nullptr) return false;

    if (mysql_stmt_reset(stmt) != 0) {
        std::cerr << "[log.cpp] [DB Error] stmt reset 실패 (" << stmt_name
                  << "): " << mysql_stmt_error(stmt) << std::endl;
        return false;
    }

    if (mysql_stmt_bind_param(stmt, bind) != 0) {
        std::cerr << "[log.cpp] [DB Error] stmt bind 실패 (" << stmt_name
                  << "): " << mysql_stmt_error(stmt) << std::endl;
        return false;
    }

    if (mysql_stmt_execute(stmt) != 0) {
        std::cerr << "[log.cpp] [DB Error] stmt execute 실패 (" << stmt_name
                  << "): " << mysql_stmt_error(stmt) << std::endl;
        return false;
    }

    return true;
}

int execute_delete(MYSQL* conn, const char* query, my_ulonglong* affected_rows) {
    if (mysql_query(conn, query) != 0) {
        return mysql_errno(conn);
    }
    if (affected_rows != nullptr) {
        *affected_rows = mysql_affected_rows(conn);
    }
    return 0;
}

std::string normalize_to_iso8601(std::string timestamp) {
    if (timestamp.size() >= 19 && timestamp[10] == ' ') {
        timestamp[10] = 'T';
        timestamp.resize(19);
    }
    return timestamp;
}

bool fetch_recording_info_by_id(MYSQL* conn,
                                unsigned long long insert_id,
                                VideoCatalogRecordInfo& out_record) {
    if (conn == nullptr || insert_id == 0) return false;

    const std::string query =
        "SELECT id, filename, DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') "
        "FROM recordings WHERE id=" +
        std::to_string(insert_id) + " LIMIT 1";
    if (mysql_query(conn, query.c_str()) != 0) {
        std::cerr << "[log.cpp] [DB Error] recordings 조회 실패: " << mysql_error(conn)
                  << std::endl;
        return false;
    }

    MYSQL_RES* res = mysql_store_result(conn);
    if (res == nullptr) {
        std::cerr << "[log.cpp] [DB Error] recordings 결과 조회 실패." << std::endl;
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(res);
    if (row == nullptr || row[0] == nullptr || row[1] == nullptr || row[2] == nullptr) {
        mysql_free_result(res);
        return false;
    }

    out_record.id = std::strtoll(row[0], nullptr, 10);
    out_record.filename = row[1];
    out_record.created_at = normalize_to_iso8601(row[2]);
    mysql_free_result(res);
    return out_record.id > 0 && !out_record.filename.empty() && !out_record.created_at.empty();
}

void bind_login_params(const LogItem& item,
                       const std::string& status,
                       MYSQL_BIND (&params)[3],
                       unsigned long (&lengths)[3]) {
    std::memset(params, 0, sizeof(params));
    lengths[0] = static_cast<unsigned long>(item.str1.size());
    lengths[1] = static_cast<unsigned long>(item.str2.size());
    lengths[2] = static_cast<unsigned long>(status.size());

    params[0].buffer_type = MYSQL_TYPE_STRING;
    params[0].buffer = const_cast<char*>(item.str1.c_str());
    params[0].buffer_length = lengths[0];
    params[0].length = &lengths[0];

    params[1].buffer_type = MYSQL_TYPE_STRING;
    params[1].buffer = const_cast<char*>(item.str2.c_str());
    params[1].buffer_length = lengths[1];
    params[1].length = &lengths[1];

    params[2].buffer_type = MYSQL_TYPE_STRING;
    params[2].buffer = const_cast<char*>(status.c_str());
    params[2].buffer_length = lengths[2];
    params[2].length = &lengths[2];
}

void bind_recording_params(const LogItem& item,
                           MYSQL_BIND (&params)[1],
                           unsigned long& filename_len) {
    std::memset(params, 0, sizeof(params));
    filename_len = static_cast<unsigned long>(item.str1.size());
    params[0].buffer_type = MYSQL_TYPE_STRING;
    params[0].buffer = const_cast<char*>(item.str1.c_str());
    params[0].buffer_length = filename_len;
    params[0].length = &filename_len;
}

void run_cleanup_queries(MYSQL* conn) {
    execute_delete(conn, kAnalyticsRetentionDeleteQuery, nullptr);

    execute_delete(conn, kLoginLogRetentionDeleteQuery, nullptr);

    const int recording_cleanup_err =
        execute_delete(conn, kRecordingRetentionDeleteByCreatedAtQuery, nullptr);
    if (recording_cleanup_err == 1054) {
        execute_delete(conn, kRecordingRetentionDeleteByFilenameQuery, nullptr);
    }
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
                const std::string status = item.login_success ? "SUCCESS" : "FAIL";
                unsigned long lengths[3];
                bind_login_params(item, status, params, lengths);
                execute_stmt(loginLogStmt, params, "login_logs");
            } else if (item.type == RECORDING_LOG) {
                MYSQL_BIND params[1];
                unsigned long filename_len = 0;
                bind_recording_params(item, params, filename_len);
                if (!execute_stmt(recordingStmt, params, "recordings")) {
                    continue;
                }

                VideoCatalogRecordInfo record_info;
                if (!fetch_recording_info_by_id(conn, mysql_insert_id(conn), record_info)) {
                    continue;
                }

                std::error_code ec;
                if (!std::filesystem::exists(record_info.filename, ec) ||
                    !std::filesystem::is_regular_file(record_info.filename, ec)) {
                    continue;
                }
                publish_video_catalog_record_added(record_info);
            } else if (item.type == CLEANUP_DB_LOG) {
                cleanup_requested = true;
            }
        }

        if (!cleanup_requested) continue;
        run_cleanup_queries(conn);
    }
}
