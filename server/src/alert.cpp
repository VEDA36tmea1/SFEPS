#include "../include/alert.h"
#include <vector>
#include <mutex>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <iostream>

// declare externals from main.cpp
extern std::vector<int> g_client_sockets;
extern std::mutex g_sockets_mutex;

void send_alert_to_clients(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_sockets_mutex);
    for (auto it = g_client_sockets.begin(); it != g_client_sockets.end(); ) {
        int fd = *it;
        ssize_t n = send(fd, msg.c_str(), msg.size(), 0);
        if (n <= 0) {
            close(fd);
            it = g_client_sockets.erase(it);
        } else {
            ++it;
        }
    }
    std::cout << "[Alert] Sent to clients: " << msg << std::endl;
}
