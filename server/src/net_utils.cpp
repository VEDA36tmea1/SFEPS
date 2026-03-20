#include "net_utils.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

bool is_ip_allowed(const std::unordered_set<std::string>& allowlist, const std::string& client_ip) {
    if (allowlist.empty()) return false;
    return allowlist.find(client_ip) != allowlist.end();
}

std::string peer_ip_to_string(const sockaddr_in& peer_addr) {
    char ip_buf[INET_ADDRSTRLEN] = {0};
    const char* ip_res =
        inet_ntop(AF_INET, &peer_addr.sin_addr, ip_buf, static_cast<socklen_t>(sizeof(ip_buf)));
    return (ip_res != nullptr) ? std::string(ip_buf) : std::string("Unknown_IP");
}

void apply_socket_read_timeout(int fd, int timeout_ms) {
    if (fd < 0 || timeout_ms <= 0) return;

    timeval tv {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        std::cerr << "[main.cpp] [Security] SO_RCVTIMEO 설정 실패: " << std::strerror(errno)
                  << std::endl;
    }
}

int create_listen_socket(int port, const char* tag, const std::string& bind_ip) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "[" << tag << "] socket() 실패: " << std::strerror(errno) << std::endl;
        return -1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        std::cerr << "[" << tag << "] setsockopt(SO_REUSEADDR) 실패: " << std::strerror(errno)
                  << std::endl;
        close(server_fd);
        return -1;
    }

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "[" << tag << "] 잘못된 bind IP: " << bind_ip << std::endl;
        close(server_fd);
        return -1;
    }

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[" << tag << "] bind() 실패: " << std::strerror(errno) << std::endl;
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 16) != 0) {
        std::cerr << "[" << tag << "] listen() 실패: " << std::strerror(errno) << std::endl;
        close(server_fd);
        return -1;
    }

    return server_fd;
}

bool send_all_plain(int fd, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
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

bool send_all_tls(const TlsClientConnection& client, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = tls_write(client, data + sent, len - sent);
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
