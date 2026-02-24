#include "auth.h"
#include "db_tls.h"
#include <cstring>
#include <iostream>

namespace {
constexpr const char* kAuthQuery = "SELECT 1 FROM users WHERE id = ? AND password = ? LIMIT 1";
} // namespace

Authenticator::Authenticator(const char* h, const char* u, const char* p, const char* db)
    : host(h), user(u), pass(p), db_name(db), conn(NULL), authStmt(NULL) {}

Authenticator::~Authenticator() {
    if (authStmt != NULL) {
        mysql_stmt_close(authStmt);
    }
    if (conn != NULL) {
        mysql_close(conn);
    }
}

bool Authenticator::connect() {
    conn = mysql_init(NULL);
    if (conn == NULL) return false;

    std::string tls_err;
    if (!configure_db_tls(conn, "auth", tls_err)) {
        std::cerr << "[Auth DB TLS Error] " << tls_err << std::endl;
        mysql_close(conn);
        conn = NULL;
        return false;
    }

    if (mysql_real_connect(conn, host, user, pass, db_name, 3306, NULL, 0) == NULL) {
        std::cerr << "[Auth DB Error] " << mysql_error(conn) << std::endl;
        mysql_close(conn);
        conn = NULL;
        return false;
    }

    authStmt = mysql_stmt_init(conn);
    if (authStmt == NULL) {
        std::cerr << "[Auth DB Error] mysql_stmt_init() failed" << std::endl;
        mysql_close(conn);
        conn = NULL;
        return false;
    }

    if (mysql_stmt_prepare(authStmt, kAuthQuery, std::strlen(kAuthQuery)) != 0) {
        std::cerr << "[Auth DB Error] prepare failed: " << mysql_stmt_error(authStmt) << std::endl;
        mysql_stmt_close(authStmt);
        authStmt = NULL;
        mysql_close(conn);
        conn = NULL;
        return false;
    }

    return true;
}

bool Authenticator::authenticate(const std::string& id, const std::string& pw) {
    if (conn == NULL || authStmt == NULL) return false;

    std::lock_guard<std::mutex> dbLock(dbMutex);

    if (mysql_stmt_reset(authStmt) != 0) {
        std::cerr << "[Auth DB Error] reset failed: " << mysql_stmt_error(authStmt) << std::endl;
        return false;
    }

    MYSQL_BIND params[2];
    std::memset(params, 0, sizeof(params));

    unsigned long id_len = static_cast<unsigned long>(id.size());
    unsigned long pw_len = static_cast<unsigned long>(pw.size());

    params[0].buffer_type = MYSQL_TYPE_STRING;
    params[0].buffer = (void*)id.c_str();
    params[0].buffer_length = static_cast<unsigned long>(id.size());
    params[0].is_null = NULL;
    params[0].length = &id_len;

    params[1].buffer_type = MYSQL_TYPE_STRING;
    params[1].buffer = (void*)pw.c_str();
    params[1].buffer_length = static_cast<unsigned long>(pw.size());
    params[1].is_null = NULL;
    params[1].length = &pw_len;

    if (mysql_stmt_bind_param(authStmt, params) != 0) {
        std::cerr << "[Auth DB Error] bind failed: " << mysql_stmt_error(authStmt) << std::endl;
        return false;
    }

    if (mysql_stmt_execute(authStmt) != 0) {
        std::cerr << "[Auth DB Error] execute failed: " << mysql_stmt_error(authStmt) << std::endl;
        return false;
    }

    if (mysql_stmt_store_result(authStmt) != 0) {
        std::cerr << "[Auth DB Error] store_result failed: " << mysql_stmt_error(authStmt) << std::endl;
        return false;
    }

    my_ulonglong rowCount = mysql_stmt_num_rows(authStmt);
    mysql_stmt_free_result(authStmt);

    return rowCount > 0;
}
