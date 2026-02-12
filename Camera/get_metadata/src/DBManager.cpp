#include "DBManager.h"
#include <cstdio> // sprintf 사용

DBManager::DBManager() {
    conn = mysql_init(NULL);
}

DBManager::~DBManager() {
    disconnect();
}

bool DBManager::connect() {
    if (!mysql_real_connect(conn, host, user, pass, db_name, port, NULL, 0)) {
        std::cerr << "🔥 DB 연결 실패: " << mysql_error(conn) << std::endl;
        return false;
    }
    std::cout << "✅ MariaDB 연결 성공!" << std::endl;
    return true;
}

void DBManager::disconnect() {
    if (conn) {
        mysql_close(conn);
        conn = NULL;
    }
}

bool DBManager::insertLog(std::string obj_id, std::string type, float x, float y, unsigned int rtp) {
    if (!conn) return false;

    char query[512];
    // SQL 쿼리 만들기: INSERT INTO 테이블명 VALUES (...)
    sprintf(query, 
        "INSERT INTO access_logs (object_id, obj_type, pos_x, pos_y, rtp_timestamp) VALUES ('%s', '%s', %.4f, %.4f, %u)", 
        obj_id.c_str(), type.c_str(), x, y, rtp);

    // 쿼리 전송
    if (mysql_query(conn, query)) {
        std::cerr << "❌ 데이터 저장 실패: " << mysql_error(conn) << std::endl;
        return false;
    }
    return true; // 성공
}
