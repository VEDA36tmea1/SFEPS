#ifndef LOG_H
#define LOG_H

#include <string>
#include <mysql/mysql.h>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <tinyxml2.h> // XML 파싱 라이브러리

// 로그 종류 구분
enum LogType { SYSTEM_LOG, LOGIN_LOG, ANALYTICS_LOG, RECORDING_LOG, CLEANUP_DB_LOG };

// 큐에 담을 데이터 구조체
struct LogItem {
    LogType type;
    
    // 공통 데이터 (message, details, username 등)
    std::string str1;
    std::string str2;
    
    // 분석/로그인 전용 데이터
    std::string time_str;   // frame_time
    float value;            // confidence 또는 login_status
};

class DBLogger {
public:
    DBLogger(const char* db = "CCgbd");
    ~DBLogger();
    
    // DB 연결
    bool connect();
    
    // 1. 일반 시스템 로그 저장
    void enqueue(const std::string& type, const std::string& message);

    // 2. 로그인 기록 저장
    void enqueueLogin(const std::string& username, const std::string& ip, bool success);

    // 3. 분석 데이터 저장 (내부적으로 사용됨)
    void enqueueAnalytics(const std::string& time, const std::string& objType, float conf, const std::string& details);
    
    // [핵심] XML 문자열을 받아서 파싱 후 DB에 저장하는 함수
    void parseAndLogXML(const char* xmlData);

    // 4. [신규] 녹화 파일 기록 함수 (이게 없어서 에러난 것임)
    void enqueueRecording(const std::string& filename);

    // [신규] DB 용량 관리(청소) 요청 함수
    void requestDbCleanup();


private:
    void processQueue(); // 백그라운드 일꾼 스레드

    MYSQL* conn;
    
    // ▼▼▼ 본인 DB 설정에 맞게 수정 필수 ▼▼▼
    const char* host = "192.168.0.92";
    const char* user = "pi";
    const char* pass = "raspberry";     // 비밀번호
    const char* db_name = "CCgbd"; // DB 이름 (sfeps_db로 바꿨으면 수정)

    // 스레드 관련 변수
    std::queue<LogItem> logQueue;
    std::mutex queueMutex;
    std::condition_variable cv;
    std::thread workerThread;
    std::atomic<bool> isRunning;
};


#endif