#include "alert.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

std::vector<int> g_plain_clients;
std::vector<TlsClientConnection> g_tls_clients;
std::mutex g_alert_clients_mutex;

bool send_all_plain(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        return false;
    }
    return true;
}

bool send_all_tls(const TlsClientConnection& client, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = tls_write(client, data + sent, len - sent);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        return false;
    }
    return true;
}

std::string describe_plain_peer(int fd) {
    sockaddr_in addr {};
    socklen_t addrlen = sizeof(addr);
    std::string peer = "fd=" + std::to_string(fd);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &addrlen) == 0) {
        peer += " peer=" + std::string(inet_ntoa(addr.sin_addr)) + ":" +
                std::to_string(ntohs(addr.sin_port));
    }
    return peer;
}

std::string describe_tls_peer(const TlsClientConnection& client) {
    sockaddr_in addr {};
    socklen_t addrlen = sizeof(addr);
    std::string peer = "tls_fd=" + std::to_string(client.fd);
    if (client.fd >= 0 && getpeername(client.fd, reinterpret_cast<sockaddr*>(&addr), &addrlen) == 0) {
        peer += " peer=" + std::string(inet_ntoa(addr.sin_addr)) + ":" +
                std::to_string(ntohs(addr.sin_port));
    }
    return peer;
}

}  // namespace

bool add_alert_plain_client(int fd) {
    if (fd < 0) return false;
    std::lock_guard<std::mutex> lock(g_alert_clients_mutex);
    g_plain_clients.push_back(fd);
    return true;
}

bool add_alert_tls_client(TlsClientConnection&& client) {
    if (client.fd < 0 || client.ssl == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_alert_clients_mutex);
    g_tls_clients.emplace_back(std::move(client));
    return true;
}

std::size_t alert_client_count() {
    std::lock_guard<std::mutex> lock(g_alert_clients_mutex);
    return g_plain_clients.size() + g_tls_clients.size();
}

void close_alert_client_connections() {
    std::lock_guard<std::mutex> lock(g_alert_clients_mutex);
    for (int fd : g_plain_clients) {
        close(fd);
    }
    g_plain_clients.clear();

    for (auto& client : g_tls_clients) {
        close_tls_client(client);
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
        std::cout << "[alert.cpp] [Alert] No connected clients. Skip sending." << std::endl;
        return;
    }

    size_t sent_cnt = 0;
    size_t fail_cnt = 0;

    for (auto it = g_plain_clients.begin(); it != g_plain_clients.end();) {
        const int fd = *it;
        if (!send_all_plain(fd, msg.data(), payload_len)) {
            std::cout << "[alert.cpp] [Alert] send failed (" << describe_plain_peer(fd)
                      << ") err=" << errno << " (" << std::strerror(errno) << ")" << std::endl;
            close(fd);
            it = g_plain_clients.erase(it);
            ++fail_cnt;
            continue;
        }
        ++sent_cnt;
        ++it;
    }

    for (auto it = g_tls_clients.begin(); it != g_tls_clients.end();) {
        if (!send_all_tls(*it, msg.data(), payload_len)) {
            std::cout << "[alert.cpp] [Alert] TLS send failed (" << describe_tls_peer(*it)
                      << ") err=" << errno << " (" << std::strerror(errno) << ")" << std::endl;
            close_tls_client(*it);
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

void send_test_alert_to_clients(const std::string& msg) {
    std::string out = msg;
    if (out.empty()) return;
    if (out.back() != '\n') out.push_back('\n');
    send_alert_to_clients(out);
}
