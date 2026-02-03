#ifndef DB_HANDLER_H
#define DB_HANDLER_H

#include <string>
#include <mysql/mysql.h>

class DBHandler {
public:
    DBHandler();
    ~DBHandler();
    
    // DB 연결 함수
    bool connect();
    // 로그 저장 함수
    void writeLog(const std::string& message);

private:
    MYSQL* conn;
    const char* host = "localhost";
    const char* user = "pi";
    const char* pass = "raspberry"; // 비밀번호 확인!
    const char* db_name = "iot_project";
};

#endif
