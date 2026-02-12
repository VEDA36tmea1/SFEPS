#ifndef RFID_MONITOR_H
#define RFID_MONITOR_H

#include <string>
#include <atomic>
#include <thread>
#include <vector>

// DB 연결 정보가 필요하므로 main에 있는 설정을 가져오거나
// 생성자로 받도록 합니다.
class RfidMonitor {
public:
    // 생성자: 종료 플래그와 DB 설정을 받음
    RfidMonitor(std::atomic<bool>& running_flag, 
                const std::string& db_host, 
                const std::string& db_user, 
                const std::string& db_pass, 
                const std::string& db_name);
    
    ~RfidMonitor();

    // 스레드 시작 함수
    void start();

private:
    std::atomic<bool>& m_running; // 메인 스레드의 종료 플래그 참조
    std::string m_socket_path;
    
    // DB 설정
    std::string m_db_host;
    std::string m_db_user;
    std::string m_db_pass;
    std::string m_db_name;

    // 내부 동작 함수
    void run_loop();
    std::string extract_json_value(const std::string& json, const std::string& key);
    std::string get_current_datetime();
    void save_to_db(const std::string& uid, const std::string& age_group, const std::string& time_str);
};

#endif // RFID_MONITOR_H