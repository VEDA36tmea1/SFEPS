#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

static bool g_running = true;

void handle_signal(int) {
    g_running = false;
}

// 라즈베리 파이에서 0.0.0.0:PORT 로 TCP 서버를 열고,
// ESP8266(클라이언트)이 접속하면 좌표 문자열을 보내는 간단한 테스트 서버.
//
// 표준 입력으로:
//   - "0.5 0.3" 처럼 "x y" 를 입력하면  =>  "CX=0.500000,CY=0.300000\n" 을 전송
//   - 그 외 문자열을 입력하면          =>  입력 문자열 그대로 + '\n' 을 전송
//
// ESP 쪽에서는 단순히 TCP로 접속해서 수신 문자열을 확인하면 됨.

int main(int argc, char** argv) {
    int port = 5555;
    if (argc >= 2) {
        port = std::stoi(argv[1]);
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::perror("socket");
        return 1;
    }

    int opt = 1;
    ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  // 0.0.0.0
    addr.sin_port        = htons(port);

    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        ::close(server_fd);
        return 1;
    }

    if (::listen(server_fd, 5) < 0) {
        std::perror("listen");
        ::close(server_fd);
        return 1;
    }

    std::cout << "[TCP] Raspi TCP 서버 시작" << std::endl;
    std::cout << "[TCP] Listening on 0.0.0.0:" << port << std::endl;
    std::cout << "[TCP] ESP8266은 192.168.4.1:" << port << " 로 접속하면 됨." << std::endl;

    while (g_running) {
        std::cout << "[TCP] 클라이언트 연결 대기 중..." << std::endl;

        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);
        int client_fd = ::accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (!g_running) break;
            std::perror("accept");
            continue;
        }

        char client_ip[64]{};
        ::inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        std::cout << "[TCP] Client connected from " << client_ip
                  << ":" << ntohs(client_addr.sin_port) << std::endl;

        std::cout << "[TCP] 좌표 입력 예시: \"0.5 0.3\" (x y)" << std::endl;
        std::cout << "[TCP] 일반 문자열도 전송 가능, \"quit\" 입력 시 연결 종료" << std::endl;

        std::string line;
        while (g_running && std::cout << "> " && std::getline(std::cin, line)) {
            if (line == "quit" || line == "exit") {
                std::cout << "[TCP] 클라이언트 연결 종료." << std::endl;
                break;
            }

            char send_buf[128];

            // "x y" 형식이면 좌표로 해석해서 CX/CY 포맷으로 전송
            float x = 0.0f, y = 0.0f;
            if (std::sscanf(line.c_str(), "%f %f", &x, &y) == 2) {
                int len = std::snprintf(send_buf, sizeof(send_buf),
                                        "CX=%.6f,CY=%.6f\n", x, y);
                if (len <= 0 || len >= static_cast<int>(sizeof(send_buf))) {
                    std::cerr << "[TCP] 좌표 포맷 실패" << std::endl;
                    continue;
                }
                if (::send(client_fd, send_buf, len, 0) <= 0) {
                    std::perror("send");
                    break;
                }
                std::cout << "[TCP] Sent: " << send_buf;
            } else {
                // 그 외에는 입력 문자열 그대로 전송
                line.push_back('\n');
                if (::send(client_fd, line.data(), static_cast<int>(line.size()), 0) <= 0) {
                    std::perror("send");
                    break;
                }
                std::cout << "[TCP] Sent raw: " << line;
            }
        }

        ::close(client_fd);
        if (!g_running) break;
    }

    ::close(server_fd);
    std::cout << "[TCP] 서버 종료" << std::endl;
    return 0;
}

