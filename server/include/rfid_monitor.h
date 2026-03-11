#ifndef RFID_MONITOR_H
#define RFID_MONITOR_H

#include <atomic>
#include <string>

class AnalyticsProcessor;

class RfidMonitor {
public:
    RfidMonitor(std::atomic<bool>& running_flag, AnalyticsProcessor& analytics);

    ~RfidMonitor();

    // 스레드 시작 함수
    void start();

private:
    std::atomic<bool>& m_running; // 메인 스레드의 종료 플래그 참조
    AnalyticsProcessor& m_analytics;
    std::string m_socket_path;

    // 내부 동작 함수
    void run_loop();
    std::string extract_json_value(const std::string& json, const std::string& key);
    std::string get_current_datetime();
    void process_rfid_tag(const std::string& uid,
                          const std::string& card_age_text,
                          const std::string& time_str);
};

#endif  // RFID_MONITOR_H
