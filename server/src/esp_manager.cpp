#include "esp_manager.h"

#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {

bool send_all_plain(int fd, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        return false;
    }
    return true;
}

std::string peer_ip_to_string(const sockaddr_in& peer_addr) {
    char ip_buf[INET_ADDRSTRLEN] = {0};
    const char* ip_res =
        ::inet_ntop(AF_INET, &peer_addr.sin_addr, ip_buf, static_cast<socklen_t>(sizeof(ip_buf)));
    return (ip_res != nullptr) ? std::string(ip_buf) : std::string("Unknown_IP");
}

bool is_ip_allowed(const std::unordered_set<std::string>& allow_ips, const std::string& client_ip) {
    if (allow_ips.empty()) return true;
    return allow_ips.find(client_ip) != allow_ips.end();
}

constexpr const char* kStartupReadyMessage = "ESP_READY|SERVER_ONLINE\n";

}  // namespace

EspManager::EspManager(Config config) : config_(std::move(config)) {}

EspManager::~EspManager() {
    stop();
}

bool EspManager::start(std::atomic<bool>& app_running_flag) {
    if (!config_.enabled) return true;
    if (server_running_.load()) return true;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "[esp_manager.cpp] [ESP] socket() failed: " << std::strerror(errno)
                  << std::endl;
        return false;
    }

    int opt = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        std::cerr << "[esp_manager.cpp] [ESP] setsockopt(SO_REUSEADDR) failed: "
                  << std::strerror(errno) << std::endl;
        ::close(fd);
        return false;
    }

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(config_.port));
    if (::inet_pton(AF_INET, config_.bind_ip.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "[esp_manager.cpp] [ESP] invalid bind IP: " << config_.bind_ip << std::endl;
        ::close(fd);
        return false;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[esp_manager.cpp] [ESP] bind() failed: " << std::strerror(errno)
                  << " (" << config_.bind_ip << ":" << config_.port << ")" << std::endl;
        ::close(fd);
        return false;
    }

    if (::listen(fd, 8) != 0) {
        std::cerr << "[esp_manager.cpp] [ESP] listen() failed: " << std::strerror(errno)
                  << std::endl;
        ::close(fd);
        return false;
    }

    app_running_flag_ = &app_running_flag;
    server_fd_ = fd;
    server_running_ = true;
    startup_ready_announced_ = false;
    accept_thread_ = std::thread(&EspManager::acceptLoop, this);
    startup_ready_thread_ = std::thread(&EspManager::sendStartupReadyAfterDelay, this);

    std::cout << "[esp_manager.cpp] [ESP] listening on " << config_.bind_ip << ":"
              << config_.port << ", max_clients=" << config_.max_clients << std::endl;
    return true;
}

void EspManager::stop() {
    const bool was_running = server_running_.exchange(false);
    if (!was_running && server_fd_ < 0 && !accept_thread_.joinable()) {
        return;
    }

    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    if (startup_ready_thread_.joinable()) {
        startup_ready_thread_.join();
    }

    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (int fd : clients_) {
        ::close(fd);
    }
    clients_.clear();
}

bool EspManager::publishFraudBbox(const FraudBboxPayload& payload) {
    if (!server_running_.load()) return false;
    if (payload.left < 0.0f || payload.top < 0.0f || payload.right < payload.left ||
        payload.bottom < payload.top) {
        return false;
    }

    char line[256];
    const float cx = (payload.left + payload.right) * 0.5f;
    const float cy = (payload.top + payload.bottom) * 0.5f;
    const float width = payload.right - payload.left;
    const float height = payload.bottom - payload.top;
    const int line_len = std::snprintf(
        line, sizeof(line),
        "FRAUD_BBOX|%s|%s|%s|CX=%.6f|CY=%.6f|W=%.6f|H=%.6f\n",
        payload.object_id.c_str(), payload.card_age_text.c_str(), payload.age_group.c_str(),
        cx, cy, width, height);
    if (line_len <= 0 || line_len >= static_cast<int>(sizeof(line))) {
        return false;
    }

    std::lock_guard<std::mutex> lock(clients_mutex_);
    if (clients_.empty()) {
        std::cout << "[esp_manager.cpp] [ESP] no connected clients for fraud bbox: object_id="
                  << payload.object_id << std::endl;
        return false;
    }

    bool delivered = false;
    for (auto it = clients_.begin(); it != clients_.end();) {
        if (!sendLineLocked(*it, line, static_cast<std::size_t>(line_len))) {
            std::cout << "[esp_manager.cpp] [ESP] send failed, closing fd=" << *it
                      << " err=" << errno << " (" << std::strerror(errno) << ")" << std::endl;
            ::close(*it);
            it = clients_.erase(it);
            continue;
        }
        delivered = true;
        ++it;
    }

    if (delivered) {
        std::cout << "[esp_manager.cpp] [ESP] sent fraud bbox: object_id=" << payload.object_id
                  << ", clients=" << clients_.size() << std::endl;
    }
    return delivered;
}

bool EspManager::sendLineLocked(int fd, const char* data, std::size_t len) {
    return send_all_plain(fd, data, len);
}

void EspManager::sendStartupReadyAfterDelay() {
    constexpr auto kReadyDelay = std::chrono::seconds(5);
    const auto start_at = std::chrono::steady_clock::now();
    while (server_running_.load()) {
        if (app_running_flag_ != nullptr && !app_running_flag_->load()) {
            return;
        }
        if ((std::chrono::steady_clock::now() - start_at) >= kReadyDelay) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!server_running_.load()) return;

    startup_ready_announced_ = true;
    std::lock_guard<std::mutex> lock(clients_mutex_);
    if (clients_.empty()) {
        std::cout << "[esp_manager.cpp] [ESP] startup ready message queued but no clients connected."
                  << std::endl;
        return;
    }

    for (auto it = clients_.begin(); it != clients_.end();) {
        if (!sendLineLocked(*it, kStartupReadyMessage, std::strlen(kStartupReadyMessage))) {
            std::cout << "[esp_manager.cpp] [ESP] startup ready send failed, closing fd=" << *it
                      << " err=" << errno << " (" << std::strerror(errno) << ")" << std::endl;
            ::close(*it);
            it = clients_.erase(it);
            continue;
        }
        ++it;
    }

    std::cout << "[esp_manager.cpp] [ESP] startup ready message sent." << std::endl;
}

void EspManager::acceptLoop() {
    while (server_running_.load()) {
        const int fd = server_fd_;
        if (fd < 0) break;

        pollfd pfd {fd, POLLIN, 0};
        const int poll_ret = ::poll(&pfd, 1, 1000);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            if (!server_running_.load()) break;
            std::cerr << "[esp_manager.cpp] [ESP] poll() failed: " << std::strerror(errno)
                      << std::endl;
            break;
        }
        if (poll_ret == 0 || (pfd.revents & POLLIN) == 0) continue;

        sockaddr_in peer_addr {};
        socklen_t peer_len = sizeof(peer_addr);
        const int client_fd =
            ::accept(fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
        if (client_fd < 0) {
            if (!server_running_.load()) break;
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            std::cerr << "[esp_manager.cpp] [ESP] accept() failed: " << std::strerror(errno)
                      << std::endl;
            continue;
        }

        const std::string client_ip = peer_ip_to_string(peer_addr);
        if (!is_ip_allowed(config_.allow_ips, client_ip)) {
            std::cout << "[esp_manager.cpp] [ESP] reject client by allowlist: ip=" << client_ip
                      << std::endl;
            ::close(client_fd);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            if (clients_.size() >= config_.max_clients) {
                std::cout << "[esp_manager.cpp] [ESP] reject client: max_clients="
                          << config_.max_clients << std::endl;
                ::close(client_fd);
                continue;
            }
            clients_.push_back(client_fd);
            if (startup_ready_announced_.load()) {
                if (!sendLineLocked(client_fd, kStartupReadyMessage,
                                    std::strlen(kStartupReadyMessage))) {
                    std::cout << "[esp_manager.cpp] [ESP] startup ready send failed on connect, closing fd="
                              << client_fd << " err=" << errno << " (" << std::strerror(errno)
                              << ")" << std::endl;
                    ::close(client_fd);
                    clients_.pop_back();
                    continue;
                }
            }
        }

        std::cout << "[esp_manager.cpp] [ESP] client connected: ip=" << client_ip << ":"
                  << ntohs(peer_addr.sin_port) << ", fd=" << client_fd << std::endl;
    }

    std::cout << "[esp_manager.cpp] [ESP] accept loop stopped." << std::endl;
}
