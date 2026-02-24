#include "alert.h"// 1회 테스트전있던 코드

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <mutex>  // 1회 테스트전있던 코드
#include <netinet/in.h> 
#include <string>// 1회 테스트전있던 코드
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>// 1회 테스트전있던 코드

namespace {
bool send_all(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }

        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}
} // namespace

// declare externals from main.cpp
extern std::vector<int> g_client_sockets;
extern std::mutex g_sockets_mutex;

void send_alert_to_clients(const std::string& msg) {
    if (msg.empty()) return;

    std::lock_guard<std::mutex> lock(g_sockets_mutex);
    const size_t payload_len = msg.size();
    std::cout << "[alert.cpp] " << "[Alert] Dispatch start: clients=" << g_client_sockets.size()
              << ", len=" << payload_len << std::endl;

    if (g_client_sockets.empty()) {
        std::cout << "[alert.cpp] " << "[Alert] No connected clients. Skip sending." << std::endl;
        return;
    }

    size_t sent_cnt = 0;
    size_t fail_cnt = 0;

    for (auto it = g_client_sockets.begin(); it != g_client_sockets.end(); ) {
        int fd = *it;
        const bool sent_ok = send_all(fd, msg.data(), payload_len);
        if (!sent_ok) {
            sockaddr_in addr {};
            socklen_t addrlen = sizeof(addr);
            std::string peer = "fd=" + std::to_string(fd);
            if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &addrlen) == 0) {
                peer += " peer=" + std::string(inet_ntoa(addr.sin_addr)) + ":" + std::to_string(ntohs(addr.sin_port));
            }

            fail_cnt++;
            std::cout << "[alert.cpp] " << "[Alert] send failed (" << peer << ") err=" << errno << " (" << std::strerror(errno) << ")" << std::endl;
            close(fd);
            it = g_client_sockets.erase(it);
            continue;
        }

        ++sent_cnt;
        ++it;
    }

    if (sent_cnt == 0) {
        std::cout << "[alert.cpp] " << "[Alert] No data delivered to clients. success=" << sent_cnt
                  << ", fail=" << fail_cnt << ", payload_len=" << payload_len << std::endl;
    } else {
        std::cout << "[alert.cpp] " << "[Alert] Sent to clients: success=" << sent_cnt
                  << ", fail=" << fail_cnt << ", payload='" << msg << "'" << std::endl;
    }
}
// 1회 테스트
void send_test_alert_to_clients(const std::string& msg) {
    std::string out = msg;
    if (out.empty()) return;
    if (out.back() != '\n') out.push_back('\n');
    send_alert_to_clients(out);
}
