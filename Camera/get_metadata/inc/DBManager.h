#pragma once
#include <mysql/mysql.h> // 라즈베리파이 기본 라이브러리
#include <string>
#include <iostream>

class DBManager {
private:
    MYSQL* conn;
    MYSQL_RES* res;
    MYSQL_ROW row;

    const char* host = "127.0.0.1"; // 로컬호스트
    const char* user = "pi";        // 아까 만든 ID
    const char* pass = "raspberry"; // 아까 만든 비번
    const char* db_name = "gate_db";
    int port = 3306;

public:
    DBManager();
    ~DBManager();

    bool connect();
    void disconnect();
    
    // 데이터를 넣는 핵심 함수
    bool insertLog(std::string obj_id, std::string type, float x, float y, unsigned int rtp);
};
