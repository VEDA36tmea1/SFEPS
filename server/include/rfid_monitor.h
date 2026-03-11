#ifndef RFID_MONITOR_H
#define RFID_MONITOR_H

#include <atomic>
#include <string>

class RfidMonitor {
public:
    explicit RfidMonitor(std::atomic<bool>& running_flag);
    
    ~RfidMonitor();

    // 스레드 시작 함수
    void start();

private:
    std::atomic<bool>& m_running; // 메인 스레드의 종료 플래그 참조
    std::string m_socket_path;

    // 내부 동작 함수
    void run_loop();
    std::string extract_json_value(const std::string& json, const std::string& key);
    std::string get_current_datetime();
    void save_to_db(const std::string& uid, const std::string& age_group, const std::string& time_str);
};

#endif // RFID_MONITOR_H
