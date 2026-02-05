#ifndef LOG_H
#define LOG_H

#include <string>
#include <mysql/mysql.h>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

class DBLogger {
public:
    DBLogger(const char* db = "CCgbd");
    ~DBLogger();
    
    // DB 연결
    bool connect();
    
    // [비동기] 큐에 로그 넣기 (영상 처리 방해 안 함)
    void enqueue(const std::string& type, const std::string& message);

private:
    void processQueue(); // 내부 스레드가 돌리는 함수

    MYSQL* conn;
    
    // DB 설정 (수정 필요)
    const char* host = "192.168.0.92";
    const char* user = "pi";
    const char* pass = "raspberry";     // 비밀번호 변경
    const char* db_name;

    // 스레드 관련 변수들
    std::queue<std::pair<std::string, std::string>> logQueue;
    std::mutex queueMutex;
    std::condition_variable cv;
    std::thread workerThread;
    std::atomic<bool> isRunning;
};

#endif
