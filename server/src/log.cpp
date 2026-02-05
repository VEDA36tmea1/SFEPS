#include "log.h"
#include <iostream>

DBLogger::DBLogger(const char* db) : isRunning(false), conn(NULL), db_name(db) {}

DBLogger::~DBLogger() {
    isRunning = false;
    cv.notify_one(); 
    
    if (workerThread.joinable()) {
        workerThread.join();
    }

    if (conn != NULL) {
        mysql_close(conn);
        std::cout << "[System] DB Connection Closed." << std::endl;
    }
}

bool DBLogger::connect() {
    conn = mysql_init(NULL);
    if (conn == NULL) return false;

    if (mysql_real_connect(conn, host, user, pass, db_name, 3306, NULL, 0) == NULL) {
        std::cerr << "[DB Error] " << mysql_error(conn) << std::endl;
        return false;
    }
    
    std::cout << "[System] DB Connected. Worker Thread Started." << std::endl;
    
    // 스레드 시작
    isRunning = true;
    workerThread = std::thread(&DBLogger::processQueue, this);
    
    return true;
}

void DBLogger::enqueue(const std::string& type, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        logQueue.push({type, message});
    }
    cv.notify_one(); // 일꾼 깨우기
}

void DBLogger::processQueue() {
    while (isRunning) {
        std::pair<std::string, std::string> logData;

        // 큐에서 꺼내기
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            cv.wait(lock, [this] { return !logQueue.empty() || !isRunning; });

            if (!isRunning && logQueue.empty()) break;

            logData = logQueue.front();
            logQueue.pop();
        }

        // DB 저장
        if (conn) {
            std::string query = "INSERT INTO logs (event_type, message) VALUES ('" + logData.first + "', '" + logData.second + "')";
            if (mysql_query(conn, query.c_str())) {
                std::cerr << "[DB Error] " << mysql_error(conn) << std::endl;
            } else {
                // 디버깅용 출력 (나중에 주석 처리 가능)
                // std::cout << "[DB Logged] " << logData.second << std::endl;
            }
        }
    }
}