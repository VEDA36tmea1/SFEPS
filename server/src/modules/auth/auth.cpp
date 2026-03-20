#include "auth.h"

#include <cstring>
#include <iostream>

namespace {
constexpr const char* kAuthQuery = "SELECT 1 FROM users WHERE id = ? AND password = ? LIMIT 1";
} // namespace

Authenticator::Authenticator(const char* h, const char* u, const char* p, const char* db)
    : conn(nullptr),
      authStmt(nullptr),
      host(h ? h : ""),
      user(u ? u : ""),
      pass(p ? p : ""),
      db_name(db ? db : "") {}

Authenticator::~Authenticator() {
    if (authStmt != nullptr) {
        mysql_stmt_close(authStmt);
    }
    if (conn != nullptr) {
        mysql_close(conn);
    }
}

bool Authenticator::connect() {
    conn = mysql_init(nullptr);
    if (conn == nullptr) return false;

    if (mysql_real_connect(
            conn, host.c_str(), user.c_str(), pass.c_str(), db_name.c_str(), 3306, nullptr, 0) ==
        nullptr) {
        std::cerr << "[Auth DB Error] " << mysql_error(conn) << std::endl;
        mysql_close(conn);
        conn = nullptr;
        return false;
    }

    authStmt = mysql_stmt_init(conn);
    if (authStmt == nullptr) {
        std::cerr << "[Auth DB Error] mysql_stmt_init() 실패" << std::endl;
        mysql_close(conn);
        conn = nullptr;
        return false;
    }

    if (mysql_stmt_prepare(authStmt, kAuthQuery, std::strlen(kAuthQuery)) != 0) {
        std::cerr << "[Auth DB Error] prepare 실패: " << mysql_stmt_error(authStmt) << std::endl;
        mysql_stmt_close(authStmt);
        authStmt = nullptr;
        mysql_close(conn);
        conn = nullptr;
        return false;
    }

    return true;
}

bool Authenticator::authenticate(const std::string& id, const std::string& pw) {
    if (conn == nullptr || authStmt == nullptr) return false;

    std::lock_guard<std::mutex> dbLock(dbMutex);

    if (mysql_stmt_reset(authStmt) != 0) {
        std::cerr << "[Auth DB Error] reset 실패: " << mysql_stmt_error(authStmt) << std::endl;
        return false;
    }

    MYSQL_BIND params[2];
    std::memset(params, 0, sizeof(params));

    unsigned long id_len = static_cast<unsigned long>(id.size());
    unsigned long pw_len = static_cast<unsigned long>(pw.size());

    params[0].buffer_type = MYSQL_TYPE_STRING;
    params[0].buffer = (void*)id.c_str();
    params[0].buffer_length = static_cast<unsigned long>(id.size());
    params[0].is_null = nullptr;
    params[0].length = &id_len;

    params[1].buffer_type = MYSQL_TYPE_STRING;
    params[1].buffer = (void*)pw.c_str();
    params[1].buffer_length = static_cast<unsigned long>(pw.size());
    params[1].is_null = nullptr;
    params[1].length = &pw_len;

    if (mysql_stmt_bind_param(authStmt, params) != 0) {
        std::cerr << "[Auth DB Error] bind 실패: " << mysql_stmt_error(authStmt) << std::endl;
        return false;
    }

    if (mysql_stmt_execute(authStmt) != 0) {
        std::cerr << "[Auth DB Error] execute 실패: " << mysql_stmt_error(authStmt) << std::endl;
        return false;
    }

    if (mysql_stmt_store_result(authStmt) != 0) {
        std::cerr << "[Auth DB Error] store_result 실패: " << mysql_stmt_error(authStmt) << std::endl;
        return false;
    }

    my_ulonglong rowCount = mysql_stmt_num_rows(authStmt);
    mysql_stmt_free_result(authStmt);

    return rowCount > 0;
}
