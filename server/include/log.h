#ifndef LOG_H
#define LOG_H

#include <string>
#include <mysql/mysql.h>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
enum LogType { SYSTEM_LOG, LOGIN_LOG, ANALYTICS_LOG, RECORDING_LOG, CLEANUP_DB_LOG };

struct LogItem {
    LogType type;
    
    // [공통 데이터] (시스템 로그 메시지, 로그인 ID 등)
    std::string str1; 
    std::string str2; 
    
    // [분석 로그 전용 데이터]
    std::string time_str;   // frame_time (사건 발생 시간)
    
    // ★ [수정됨] 기존 value(confidence) 삭제 -> 좌표, 이벤트, 나이, 사진경로 추가
    float x = 0;              // X 좌표
    float y = 0;              // Y 좌표
    std::string event;      // 감지 이벤트 (예: intrusion)
    int age = 0;            // 추정 나이
    std::string photo_path; // 저장된 사진 경로
    
    // 로그인 로그용 (성공/실패 여부) - value 대신 별도 변수 혹은 int 재활용 가능하지만 명시적으로 둠
    bool login_success = false; 
};

class DBLogger {
public:
    DBLogger(const char* host, const char* user, const char* pass, const char* db);
    ~DBLogger();
    
    bool connect();
    
    // 1. 일반 시스템 로그
    void enqueue(const std::string& type, const std::string& message);

    // 2. 로그인 기록
    void enqueueLogin(const std::string& username, const std::string& ip, bool success);

    // 3. ★ [수정됨] 분석 데이터 저장 함수 (인자 대폭 변경)
    // (기존: time, objType, conf, details) -> (신규: time, objType, x, y, event, age, photoPath)
    void enqueueAnalytics(const std::string& time, 
                          const std::string& objType, 
                          float x, float y, 
                          const std::string& event, 
                          int age, 
                          const std::string& photoPath);

    // 4. 녹화 파일 기록
    void enqueueRecording(const std::string& filename);

    // DB 청소
    void requestDbCleanup();

private:
    void processQueue(); 
    bool prepareStatements();
    void closeStatements();

    MYSQL* conn;
    MYSQL_STMT* systemLogStmt;
    MYSQL_STMT* loginLogStmt;
    MYSQL_STMT* analyticsLogStmt;
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
    std::string cleanupSizeQuery;
};

#endif
