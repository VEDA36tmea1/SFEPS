#ifndef ESP_MANAGER_H
#define ESP_MANAGER_H

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

class EspManager {
public:
    struct Config {
        bool enabled = false;
        std::string bind_ip = "192.168.4.1";
        int port = 5565;
        std::size_t max_clients = 4;
        std::unordered_set<std::string> allow_ips;
    };

    struct FraudBboxPayload {
        std::string object_id;
        std::string card_age_text;
        std::string age_group;
        float left = -1.0f;
        float top = -1.0f;
        float right = -1.0f;
        float bottom = -1.0f;
    };

    explicit EspManager(Config config);
    ~EspManager();

    bool start(std::atomic<bool>& app_running_flag);
    void stop();
    bool publishFraudBbox(const FraudBboxPayload& payload);

private:
    void acceptLoop();
    void sendStartupReadyAfterDelay();
    bool sendLineLocked(int fd, const char* data, std::size_t len);

    Config config_;
    std::atomic<bool>* app_running_flag_ = nullptr;
    std::atomic<bool> server_running_ {false};
    std::atomic<bool> startup_ready_announced_ {false};
    int server_fd_ = -1;
    std::thread accept_thread_;
    std::thread startup_ready_thread_;
    std::vector<int> clients_;
    std::mutex clients_mutex_;
};

#endif
