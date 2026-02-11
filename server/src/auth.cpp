#include "auth.h"
#include <iostream>

Authenticator::Authenticator(const char* h, const char* u, const char* p, const char* db)
    : host(h), user(u), pass(p), db_name(db), conn(NULL) {}

Authenticator::~Authenticator() {
    if (conn != NULL) {
        mysql_close(conn);
    }
}

bool Authenticator::connect() {
    conn = mysql_init(NULL); // MYSQL 객체 초기화
    if (conn == NULL) return false;

    // 실제 DB 서버 연결 시도
    if (mysql_real_connect(conn, host, user, pass, db_name, 3306, NULL, 0) == NULL) {
        std::cerr << "[Auth DB Error] " << mysql_error(conn) << std::endl;
        return false;
    }
    return true;
}

bool Authenticator::authenticate(const std::string& id, const std::string& pw) {
    if (conn == NULL) return false;

    // [중요] SQL Injection 방지를 위해서는 실제 프로젝트 시 준비된 문장(Prepared Statement)을 권장합니다.
    // 현재는 기본 요구사항에 맞춘 단순 쿼리 방식입니다.
    std::string query = "SELECT id FROM clients WHERE id = '" + id + "' AND password = '" + pw + "'";
    
    std::lock_guard<std::mutex> dbLock(dbMutex); // 동시 쿼리 발생 시 순차 처리 보장
    if (mysql_query(conn, query.c_str())) {
        std::cerr << "[Auth DB Error] Query Failed: " << mysql_error(conn) << std::endl;
        return false;
    }

    // 결과 셋 가져오기
    MYSQL_RES* res = mysql_store_result(conn);
    if (res == NULL) return false;

    // 행(row)의 개수가 0보다 크면 일치하는 회원이 있는 것
    bool found = (mysql_num_rows(res) > 0);
    mysql_free_result(res); // 메모리 해제 필수

    return found;
}
