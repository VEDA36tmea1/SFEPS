#ifndef ESP_MANAGER_H
#define ESP_MANAGER_H

#include <atomic>
#include <chrono>
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

    struct TrackPosPayload {
        std::string object_id;
        float left = -1.0f;
        float top = -1.0f;
        float right = -1.0f;
        float bottom = -1.0f;
        float x = -1.0f;
        float y = -1.0f;
        std::string tag_time;
    };

    explicit EspManager(Config config);
    ~EspManager();

    bool start(std::atomic<bool>& app_running_flag);
    void stop();
    void setClientTrackObjectId(const std::string& object_id);
    void clearClientTrackObjectId();
    bool publishFraudTrackPosIfIdle(const TrackPosPayload& payload);
    bool expireFraudTrackIfStale(std::chrono::seconds max_idle);
    bool publishTrackChangeSignal(const std::string& from_object_id,
                                  const std::string& to_object_id);
    bool publishTrackStart(const std::string& object_id);
    bool publishTrackPos(const TrackPosPayload& payload);
    bool publishTrackEnd(const std::string& object_id, const std::string& reason);

private:
    void acceptLoop();
    void sendStartupReadyAfterDelay();
    bool sendLineLocked(int fd, const char* data, std::size_t len);
    bool broadcastLineLocked(const char* data, std::size_t len, bool verbose_error_log);

    Config config_;
    std::atomic<bool>* app_running_flag_ = nullptr;
    std::atomic<bool> server_running_ {false};
    std::atomic<bool> startup_ready_announced_ {false};
    int server_fd_ = -1;
    std::thread accept_thread_;
    std::thread startup_ready_thread_;
    std::vector<int> clients_;
    std::mutex clients_mutex_;
    std::string client_track_object_id_;
    std::string fraud_track_object_id_;
    std::chrono::steady_clock::time_point fraud_track_last_sent_at_;
};

#endif
