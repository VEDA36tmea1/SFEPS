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
    const size_t payload_len = msg.size();
    const size_t total_clients = g_plain_clients.size() + g_tls_clients.size();

    std::cout << "[alert.cpp] [Alert] Dispatch start: clients=" << total_clients
              << ", len=" << payload_len << std::endl;

    if (total_clients == 0) {
        return;
    }

    size_t sent_cnt = 0;
    size_t fail_cnt = 0;

    for (auto it = g_plain_clients.begin(); it != g_plain_clients.end();) {
        if (!send_all_plain(it->fd, msg.data(), payload_len)) {
            std::cout << "[alert.cpp] [Alert] send failed (" << describe_plain_peer(*it)
                      << ") err=" << errno << " (" << std::strerror(errno) << ")" << std::endl;
            close(it->fd);
            it = g_plain_clients.erase(it);
            ++fail_cnt;
            continue;
        }
        ++sent_cnt;
        ++it;
    }

    for (auto it = g_tls_clients.begin(); it != g_tls_clients.end();) {
        if (!send_all_tls(it->conn, msg.data(), payload_len)) {
            std::cout << "[alert.cpp] [Alert] TLS send failed (" << describe_tls_peer(*it)
                      << ") err=" << errno << " (" << std::strerror(errno) << ")" << std::endl;
            close_tls_client(it->conn);
            it = g_tls_clients.erase(it);
            ++fail_cnt;
            continue;
        }
        ++sent_cnt;
        ++it;
    }

    if (sent_cnt == 0) {
        std::cout << "[alert.cpp] [Alert] No data delivered. success=" << sent_cnt
                  << ", fail=" << fail_cnt << ", payload_len=" << payload_len << std::endl;
    } else {
        std::cout << "[alert.cpp] [Alert] Sent to clients: success=" << sent_cnt
                  << ", fail=" << fail_cnt << ", payload='" << msg << "'" << std::endl;
    }
}

void send_alert_to_ip_clients(const std::string& ip, const std::string& msg) {
    if (ip.empty() || msg.empty()) return;

    std::lock_guard<std::mutex> lock(g_alert_clients_mutex);
    const size_t payload_len = msg.size();
    size_t target_clients = 0;
    for (const auto& client : g_plain_clients) {
        if (client.client_ip == ip) ++target_clients;
    }
    for (const auto& client : g_tls_clients) {
        if (client.client_ip == ip) ++target_clients;
    }

    std::cout << "[alert.cpp] [Alert] Target dispatch start: ip=" << ip
              << ", targets=" << target_clients << ", len=" << payload_len << std::endl;

    if (target_clients == 0) {
        return;
    }

    size_t sent_cnt = 0;
    size_t fail_cnt = 0;

    for (auto it = g_plain_clients.begin(); it != g_plain_clients.end();) {
        if (it->client_ip != ip) {
            ++it;
            continue;
        }
        if (!send_all_plain(it->fd, msg.data(), payload_len)) {
            std::cout << "[alert.cpp] [Alert] target send failed (" << describe_plain_peer(*it)
                      << ") err=" << errno << " (" << std::strerror(errno) << ")" << std::endl;
            close(it->fd);
            it = g_plain_clients.erase(it);
            ++fail_cnt;
            continue;
        }
        ++sent_cnt;
        ++it;
    }

    for (auto it = g_tls_clients.begin(); it != g_tls_clients.end();) {
        if (it->client_ip != ip) {
            ++it;
            continue;
        }
        if (!send_all_tls(it->conn, msg.data(), payload_len)) {
            std::cout << "[alert.cpp] [Alert] target TLS send failed (" << describe_tls_peer(*it)
                      << ") err=" << errno << " (" << std::strerror(errno) << ")" << std::endl;
            close_tls_client(it->conn);
            it = g_tls_clients.erase(it);
            ++fail_cnt;
            continue;
        }
        ++sent_cnt;
        ++it;
    }

    std::cout << "[alert.cpp] [Alert] Target dispatch done: ip=" << ip
              << ", success=" << sent_cnt << ", fail=" << fail_cnt
              << ", payload='" << msg << "'" << std::endl;
}
