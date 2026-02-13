#pragma once
#include <mysql/mysql.h> 
#include <string>
#include <iostream>

class DBManager {
private:
    MYSQL* conn;
    MYSQL_RES* res;
    MYSQL_ROW row;

    const char* host = "127.0.0.1"; // 로컬호스트
    const char* user = "pi";       
    const char* pass = "raspberry"; 
    const char* db_name = "gate_db";
    int port = 3306;

public:
    DBManager();
    ~DBManager();

    bool connect();
    void disconnect();
    
    bool insertLog(std::string obj_id, std::string type, float x, float y, unsigned int rtp);
};
