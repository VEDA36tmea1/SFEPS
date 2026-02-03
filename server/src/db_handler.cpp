#include "db_handler.h"
#include <iostream>

DBHandler::DBHandler() {
    conn = mysql_init(NULL);
}

DBHandler::~DBHandler() {
    if (conn) {
        mysql_close(conn);
    }
}

bool DBHandler::connect() {
    if (!mysql_real_connect(conn, host, user, pass, db_name, 0, NULL, 0)) {
        std::cerr << "[DB Error] 연결 실패: " << mysql_error(conn) << std::endl;
        return false;
    }
    std::cout << "[DB Success] 데이터베이스 연결됨." << std::endl;
    return true;
}

void DBHandler::writeLog(const std::string& message) {
    if (!conn) return;

    // 쿼리 생성
    std::string query = "INSERT INTO access_logs (user_name, status) VALUES ('System', '" + message + "')";
    
    if (mysql_query(conn, query.c_str())) {
        std::cerr << "[DB Error] 로그 저장 실패: " << mysql_error(conn) << std::endl;
    } else {
        std::cout << "[DB Log] 기록됨: " << message << std::endl;
    }
}
