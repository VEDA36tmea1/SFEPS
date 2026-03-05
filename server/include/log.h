#ifndef LOG_H
#define LOG_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <mysql/mysql.h>
#include <queue>
#include <string>
#include <thread>

enum LogType { LOGIN_LOG, RECORDING_LOG, CLEANUP_DB_LOG };

struct LogItem {
    LogType type;

    // 공통 문자열 필드:
    // - LOGIN_LOG: str1=username, str2=ip
    // - RECORDING_LOG: str1=filename
    std::string str1;
    std::string str2;

    // LOGIN_LOG 전용
    bool login_success = false;
};

class DBLogger {
public:
    DBLogger(const char* host, const char* user, const char* pass, const char* db);
    ~DBLogger();
    
    bool connect();
    
    // 1. 로그인 기록
    void enqueueLogin(const std::string& username, const std::string& ip, bool success);

    // 2. 녹화 파일 기록
    void enqueueRecording(const std::string& filename);

    // DB 청소
    void requestDbCleanup();

private:
    void processQueue(); 
    bool prepareStatements();
    void closeStatements();

    MYSQL* conn;
    MYSQL_STMT* loginLogStmt;
    MYSQL_STMT* recordingStmt;
    std::string host;
    std::string user;
    std::string pass;
    std::string db_name;

    std::queue<LogItem> logQueue;
    std::mutex queueMutex;
    std::condition_variable cv;
    std::thread workerThread;
    std::atomic<bool> isRunning;
};

#endif
