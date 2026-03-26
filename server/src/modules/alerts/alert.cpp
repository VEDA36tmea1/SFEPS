#include "alert.h"
#include "net_utils.h"
#include "tls_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

struct PlainAlertClient {
    int fd = -1;
    std::string client_ip;
};

struct TlsAlertClient {
    TlsClientConnection conn {};
    std::string client_ip;
};

std::vector<PlainAlertClient> g_plain_clients;
std::vector<TlsAlertClient> g_tls_clients;
std::mutex g_alert_clients_mutex;

struct AlertDispatchStats {
    size_t target_count = 0;
    size_t sent_count = 0;
    size_t fail_count = 0;
};

bool matches_target_ip(const std::string& client_ip, const std::string* target_ip) {
    return target_ip == nullptr || client_ip == *target_ip;
}

size_t count_targets(const std::string* target_ip) {
    size_t count = 0;
    for (const auto& client : g_plain_clients) {
        if (matches_target_ip(client.client_ip, target_ip)) ++count;
    }
    for (const auto& client : g_tls_clients) {
        if (matches_target_ip(client.client_ip, target_ip)) ++count;
    }
    return count;
}

std::string describe_plain_peer(const PlainAlertClient& client) {
    sockaddr_in addr {};
    socklen_t addrlen = sizeof(addr);
    std::string peer = "fd=" + std::to_string(client.fd);
    if (!client.client_ip.empty()) {
        peer += " ip=" + client.client_ip;
    }
    if (getpeername(client.fd, reinterpret_cast<sockaddr*>(&addr), &addrlen) == 0) {
        peer += " peer=" + std::string(inet_ntoa(addr.sin_addr)) + ":" +
                std::to_string(ntohs(addr.sin_port));
    }
    return peer;
}

std::string describe_tls_peer(const TlsAlertClient& client) {
    sockaddr_in addr {};
    socklen_t addrlen = sizeof(addr);
    std::string peer = "tls_fd=" + std::to_string(client.conn.fd);
    if (!client.client_ip.empty()) {
        peer += " ip=" + client.client_ip;
    }
    if (client.conn.fd >= 0 &&
        getpeername(client.conn.fd, reinterpret_cast<sockaddr*>(&addr), &addrlen) == 0) {
        peer += " peer=" + std::string(inet_ntoa(addr.sin_addr)) + ":" +
                std::to_string(ntohs(addr.sin_port));
    }
    return peer;
}

void dispatch_plain_alerts(const std::string& msg,
                           const std::string* target_ip,
                           AlertDispatchStats& stats,
                           const char* fail_prefix) {
    for (auto it = g_plain_clients.begin(); it != g_plain_clients.end();) {
        if (!matches_target_ip(it->client_ip, target_ip)) {
            ++it;
            continue;
        }
        ++stats.target_count;
        if (!send_all_plain(it->fd, msg.data(), msg.size())) {
            std::cout << "[alert.cpp] [Alert] " << fail_prefix << " (" << describe_plain_peer(*it)
                      << ") 오류=" << errno << " (" << std::strerror(errno) << ")" << std::endl;
            close(it->fd);
            it = g_plain_clients.erase(it);
            ++stats.fail_count;
            continue;
        }
        ++stats.sent_count;
        ++it;
    }
}

void dispatch_tls_alerts(const std::string& msg,
                         const std::string* target_ip,
                         AlertDispatchStats& stats,
                         const char* fail_prefix) {
    for (auto it = g_tls_clients.begin(); it != g_tls_clients.end();) {
        if (!matches_target_ip(it->client_ip, target_ip)) {
            ++it;
            continue;
        }
        ++stats.target_count;
        if (!send_all_tls(it->conn, msg.data(), msg.size())) {
            std::cout << "[alert.cpp] [Alert] " << fail_prefix << " (" << describe_tls_peer(*it)
                      << ") 오류=" << errno << " (" << std::strerror(errno) << ")" << std::endl;
            close_tls_client(it->conn);
            it = g_tls_clients.erase(it);
            ++stats.fail_count;
            continue;
        }
        ++stats.sent_count;
        ++it;
    }
}

}  // namespace

bool add_alert_plain_client(int fd, const std::string& client_ip) {
    if (fd < 0) return false;
    std::lock_guard<std::mutex> lock(g_alert_clients_mutex);
    PlainAlertClient client;
    client.fd = fd;
    client.client_ip = client_ip;
    g_plain_clients.push_back(std::move(client));
    return true;
}

bool add_alert_tls_client(TlsClientConnection&& client, const std::string& client_ip) {
    if (client.fd < 0 || client.ssl == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_alert_clients_mutex);
    TlsAlertClient state;
    state.conn = std::move(client);
    state.client_ip = client_ip;
    g_tls_clients.emplace_back(std::move(state));
    return true;
}

std::size_t alert_client_count() {
    std::lock_guard<std::mutex> lock(g_alert_clients_mutex);
    return g_plain_clients.size() + g_tls_clients.size();
}

void close_alert_client_connections() {
    std::lock_guard<std::mutex> lock(g_alert_clients_mutex);
    for (auto& client : g_plain_clients) {
        close(client.fd);
    }
    g_plain_clients.clear();

    for (auto& client : g_tls_clients) {
        close_tls_client(client.conn);
    }
    g_tls_clients.clear();
}

void send_alert_to_clients(const std::string& msg) {
    if (msg.empty()) return;

    std::lock_guard<std::mutex> lock(g_alert_clients_mutex);
    const size_t total_clients_before = g_plain_clients.size() + g_tls_clients.size();

    if (total_clients_before == 0) {
        return;
    }

    AlertDispatchStats stats;
    dispatch_plain_alerts(msg, nullptr, stats, "전송 실패");
    dispatch_tls_alerts(msg, nullptr, stats, "TLS 전송 실패");

    if (stats.sent_count == 0) {
        std::cout << "[alert.cpp] [Alert] 데이터 전달 없음. 성공=" << stats.sent_count
                  << ", 실패=" << stats.fail_count << ", payload_len=" << msg.size()
                  << std::endl;
    } else {
        std::cout << "[alert.cpp] [Alert] 클라이언트 전송 완료: 성공=" << stats.sent_count
                  << ", 실패=" << stats.fail_count << ", payload='" << msg << "'" << std::endl;
    }
}

void send_alert_to_ip_clients(const std::string& ip, const std::string& msg) {
    if (ip.empty() || msg.empty()) return;

    std::lock_guard<std::mutex> lock(g_alert_clients_mutex);
    const size_t target_clients = count_targets(&ip);

    std::cout << "[alert.cpp] [Alert] 대상 전송 시작: ip=" << ip
              << ", 대상수=" << target_clients << ", 길이=" << msg.size() << std::endl;
    if (target_clients == 0) return;

    AlertDispatchStats stats;
    dispatch_plain_alerts(msg, &ip, stats, "대상 전송 실패");
    dispatch_tls_alerts(msg, &ip, stats, "대상 TLS 전송 실패");

    std::cout << "[alert.cpp] [Alert] 대상 전송 완료: ip=" << ip
              << ", 성공=" << stats.sent_count << ", 실패=" << stats.fail_count
              << ", payload='" << msg << "'" << std::endl;
}
