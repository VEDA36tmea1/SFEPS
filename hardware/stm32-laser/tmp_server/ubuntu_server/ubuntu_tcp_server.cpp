#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <limits>
#include <algorithm>
 
// NOTE:
// 이 서버는 원래 TCP 전용이었다.
// 지금은 실행 인자로 TCP/UDP 모드를 선택할 수 있게 확장했다.
// - TCP 모드: 기존 그대로 (클라이언트 accept 후 send/recv)
// - UDP 모드: 서버는 UDP port에 bind 후, "라즈베리가 먼저 보내는 첫 패킷"의 주소를 기억하고
//            이후 sendto()는 그 주소로 수행한다. (HELLO/PING/임의 라인)

static bool g_running = true;

// rtsp_laser_demo에 "클라이언트 연결됨" 신호 전달 (레이저 탐지/LUT 시작 조건)
static constexpr const char* LUT_CLIENT_CONNECTED_FIFO = "/tmp/lut_client_connected";

// 서버 측 LUT: 라즈베리에서 "PAN=...,TILT=..." 수신 시 rtsp_laser_demo로 전달
static constexpr const char* LUT_PWM_RESPONSE_FIFO = "/tmp/lut_pwm_response";

// TCP 클라이언트(ESP8266/STM32)로부터 수신한 데이터를 처리하는 스레드.
// PAN=...,TILT=... 라인을 FIFO 에 쓰고, 나머지는 콘솔에 출력한다.
static void recv_thread_fn(int client_fd, std::atomic<bool>& stop_flag)
{
    ::mkfifo(LUT_PWM_RESPONSE_FIFO, 0666);
    int pwm_fifo_fd = ::open(LUT_PWM_RESPONSE_FIFO, O_WRONLY | O_NONBLOCK);
    if (pwm_fifo_fd < 0)
        std::cerr << "[TCP-recv] PWM FIFO 열기 실패: " << std::strerror(errno) << "\n";

    std::string line;
    char ch = 0;

    while (!stop_flag.load())
    {
        int r = ::recv(client_fd, &ch, 1, MSG_DONTWAIT);
        if (r > 0)
        {
            if (ch == '\n' || ch == '\r')
            {
                // 콘솔 출력
                if (!line.empty())
                {
                    std::cout << "[TCP←Raspi] " << line << std::endl;

                    // PAN=...,TILT=... 라인이면 PWM FIFO에 기록 (서버 LUT 저장용)
                    bool is_pwm = (line.find("PAN=") != std::string::npos &&
                                   line.find("TILT=") != std::string::npos);
                    if (is_pwm && pwm_fifo_fd >= 0)
                    {
                        line.push_back('\n');
                        ssize_t wr = ::write(pwm_fifo_fd, line.c_str(), line.size());
                        (void)wr;
                    }
                }
                line.clear();
            }
            else if (ch != '\r')
            {
                line.push_back(ch);
            }
        }
        else if (r == 0)
        {
            break; // 클라이언트 연결 끊김
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            else
                break;
        }
    }

    if (pwm_fifo_fd >= 0) ::close(pwm_fifo_fd);
}

void handle_signal(int) {
    g_running = false;
}

// UDP 수신: 클라이언트(라즈베리)의 PAN/TILT 라인 등을 수신해 FIFO로 전달.
// - udp mode에서는 같은 UDP 소켓을 공유 recv로 쓰면 충돌나므로,
//   RTT 모드가 아닌 경우에만 별도 스레드로 recvfrom을 돌린다.
struct UdpPeer
{
    sockaddr_in addr{};
    socklen_t   addrlen{sizeof(sockaddr_in)};
    bool        valid{false};
};

static void udp_recv_thread_fn(int udp_fd, std::atomic<bool>& stop_flag, UdpPeer& peer)
{
    ::mkfifo(LUT_PWM_RESPONSE_FIFO, 0666);
    int pwm_fifo_fd = ::open(LUT_PWM_RESPONSE_FIFO, O_WRONLY | O_NONBLOCK);
    if (pwm_fifo_fd < 0)
        std::cerr << "[UDP-recv] PWM FIFO 열기 실패: " << std::strerror(errno) << "\n";

    char buf[1024];
    while (!stop_flag.load())
    {
        sockaddr_in from{};
        socklen_t fromlen = sizeof(from);
        int n = static_cast<int>(::recvfrom(udp_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                                            reinterpret_cast<sockaddr*>(&from), &fromlen));
        if (n > 0)
        {
            buf[n] = '\0';

            // 첫 패킷/새 패킷에서 peer 업데이트 (라즈베리가 먼저 HELLO를 보내야 함)
            if (!peer.valid)
            {
                peer.addr = from;
                peer.addrlen = fromlen;
                peer.valid = true;

                char ip[64]{};
                ::inet_ntop(AF_INET, &peer.addr.sin_addr, ip, sizeof(ip));
                std::cerr << "[UDP] peer learned: " << ip << ":" << ntohs(peer.addr.sin_port) << "\n";

                // rtsp_laser_demo에 "연결됨" 신호 전달 → 레이저 탐지/LUT 시작
                ::mkfifo(LUT_CLIENT_CONNECTED_FIFO, 0666);
                int conn_fd = ::open(LUT_CLIENT_CONNECTED_FIFO, O_WRONLY);
                if (conn_fd >= 0) {
                    const char* msg = "CONNECTED\n";
                    ssize_t wr = ::write(conn_fd, msg, std::strlen(msg));
                    (void)wr;
                    ::close(conn_fd);
                }
            }

            std::string line(buf);
            // UDP는 한 datagram에 여러 줄이 올 수 있어 단순 split
            size_t pos = 0;
            while (pos < line.size())
            {
                size_t eol = line.find_first_of("\r\n", pos);
                std::string one = (eol == std::string::npos) ? line.substr(pos) : line.substr(pos, eol - pos);
                if (!one.empty())
                {
                    std::cout << "[UDP←Raspi] " << one << std::endl;
                    bool is_pwm = (one.find("PAN=") != std::string::npos &&
                                   one.find("TILT=") != std::string::npos);
                    if (is_pwm && pwm_fifo_fd >= 0)
                    {
                        one.push_back('\n');
                        ssize_t wr = ::write(pwm_fifo_fd, one.c_str(), one.size());
                        (void)wr;
                    }
                }
                if (eol == std::string::npos) break;
                pos = line.find_first_not_of("\r\n", eol);
                if (pos == std::string::npos) break;
            }
        }
        else if (n == 0)
        {
            // UDP에서 0은 거의 안 옴. 그냥 sleep.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    if (pwm_fifo_fd >= 0) ::close(pwm_fifo_fd);
}

// PING/PONG RTT 측정용 헬퍼
// - 라인 단위 echo (예: STM이 PING,1,... 를 받아서 PONG,1,... 또는 그대로 돌려보낸다고 가정)
// - seq 1..N 까지 보내고, 왕복 시간(RTT)을 ms 단위로 로그/요약 출력
static void run_rtt_test(int client_fd) {
    using clock = std::chrono::steady_clock;

    const int kCount      = 20;   // PING 횟수
    const int kIntervalMs = 200;  // PING 간 간격(ms)

    long long min_ms  = std::numeric_limits<long long>::max();
    long long max_ms  = 0;
    long long sum_ms  = 0;
    int       ok_count = 0;

    std::cout << "[RTT] ---- PING/PONG RTT 테스트 시작 ----" << std::endl;
    std::cout << "[RTT] count=" << kCount
              << ", interval=" << kIntervalMs << " ms" << std::endl;

    std::string recv_line;

    for (int seq = 1; seq <= kCount && g_running; ++seq) {
        std::string msg = "PING," + std::to_string(seq) + "\n";

        auto t0 = clock::now();
        if (::send(client_fd, msg.c_str(), static_cast<int>(msg.size()), 0) <= 0) {
            std::perror("[RTT] send");
            break;
        }

        recv_line.clear();
        char ch = 0;
        while (true) {
            int r = ::recv(client_fd, &ch, 1, 0);
            if (r <= 0) {
                std::perror("[RTT] recv");
                goto done;
            }
            if (ch == '\n') {
                break;
            }
            if (ch != '\r') {
                recv_line.push_back(ch);
            }
        }

        auto t1  = clock::now();
        auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        long long rtt_ll = static_cast<long long>(rtt);
        min_ms = std::min(min_ms, rtt_ll);
        max_ms = std::max(max_ms, rtt_ll);
        sum_ms += rtt_ll;
        ++ok_count;

        std::cout << "[RTT] seq=" << seq
                  << " rtt=" << rtt << " ms"
                  << " | line=\"" << recv_line << "\""
                  << std::endl;

        std::this_thread::sleep_for(std::chrono::milliseconds(kIntervalMs));
    }

done:
    if (ok_count > 0) {
        double avg = static_cast<double>(sum_ms) / static_cast<double>(ok_count);
        std::cout << "[RTT] ---- 완료 ----" << std::endl;
        std::cout << "[RTT] count=" << ok_count
                  << ", avg=" << avg << " ms"
                  << ", min=" << min_ms << " ms"
                  << ", max=" << max_ms << " ms"
                  << std::endl;
        std::cout << "[RTT] 대략적인 편도 지연 ≈ avg/2 ≒ "
                  << (avg / 2.0) << " ms" << std::endl;
    } else {
        std::cout << "[RTT] 유효한 응답이 없어 RTT 측정 실패." << std::endl;
    }
}

// UDP RTT: peer 주소로 sendto() 후 recvfrom()으로 응답 1줄 대기
static void run_udp_rtt_test(int udp_fd, UdpPeer& peer) {
    using clock = std::chrono::steady_clock;

    const int kCount      = 20;
    const int kIntervalMs = 200;

    long long min_ms  = std::numeric_limits<long long>::max();
    long long max_ms  = 0;
    long long sum_ms  = 0;
    int       ok_count = 0;

    std::cout << "[RTT] ---- UDP PING/PONG RTT 테스트 시작 ----" << std::endl;
    std::cout << "[RTT] count=" << kCount
              << ", interval=" << kIntervalMs << " ms" << std::endl;
    std::cout << "[RTT] (UDP) 라즈베리가 먼저 한 번 HELLO/아무 패킷을 보내 peer를 알려줘야 합니다.\n";

    // peer가 아직 없으면 대기
    while (g_running && !peer.valid)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!peer.valid)
    {
        std::cout << "[RTT] peer를 얻지 못해 종료합니다.\n";
        return;
    }

    char recv_buf[1024];
    for (int seq = 1; seq <= kCount && g_running; ++seq)
    {
        std::string msg = "PING," + std::to_string(seq) + "\n";
        auto t0 = clock::now();
        if (::sendto(udp_fd, msg.c_str(), static_cast<int>(msg.size()), 0,
                     reinterpret_cast<const sockaddr*>(&peer.addr), peer.addrlen) <= 0)
        {
            std::perror("[RTT] sendto");
            break;
        }

        std::string recv_line;
        auto deadline = clock::now() + std::chrono::milliseconds(2000);
        while (clock::now() < deadline)
        {
            sockaddr_in from{};
            socklen_t fromlen = sizeof(from);
            int n = static_cast<int>(::recvfrom(udp_fd, recv_buf, sizeof(recv_buf) - 1, MSG_DONTWAIT,
                                                reinterpret_cast<sockaddr*>(&from), &fromlen));
            if (n > 0)
            {
                recv_buf[n] = '\0';
                recv_line = recv_buf;
                break;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        auto t1  = clock::now();
        auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (recv_line.empty())
        {
            std::cout << "[RTT] seq=" << seq << " timeout\n";
            continue;
        }

        long long rtt_ll = static_cast<long long>(rtt);
        min_ms = std::min(min_ms, rtt_ll);
        max_ms = std::max(max_ms, rtt_ll);
        sum_ms += rtt_ll;
        ++ok_count;

        // 첫 줄만 보여주기
        size_t eol = recv_line.find_first_of("\r\n");
        if (eol != std::string::npos) recv_line = recv_line.substr(0, eol);

        std::cout << "[RTT] seq=" << seq
                  << " rtt=" << rtt << " ms"
                  << " | line=\"" << recv_line << "\""
                  << std::endl;

        std::this_thread::sleep_for(std::chrono::milliseconds(kIntervalMs));
    }

    if (ok_count > 0) {
        double avg = static_cast<double>(sum_ms) / static_cast<double>(ok_count);
        std::cout << "[RTT] ---- 완료 ----" << std::endl;
        std::cout << "[RTT] count=" << ok_count
                  << ", avg=" << avg << " ms"
                  << ", min=" << min_ms << " ms"
                  << ", max=" << max_ms << " ms"
                  << std::endl;
        std::cout << "[RTT] 대략적인 편도 지연 ≈ avg/2 ≒ "
                  << (avg / 2.0) << " ms" << std::endl;
    } else {
        std::cout << "[RTT] 유효한 응답이 없어 RTT 측정 실패." << std::endl;
    }
}

// Ubuntu 노트북(또는 일반 리눅스 PC)에서 0.0.0.0:PORT 로 TCP 서버를 열고,
// ESP8266(클라이언트)이 접속하면 좌표 문자열을 보내는 간단한 테스트 서버.
//
// 표준 입력으로:
//   - "0.5 0.3" 처럼 "x y" 를 입력하면  =>  "CX=0.500000,CY=0.300000\n" 을 전송
//   - 그 외 문자열을 입력하면          =>  입력 문자열 그대로 + '\n' 을 전송
//
// ESP 쪽에서는 단순히 TCP로 접속해서 수신 문자열을 확인하면 됨.

int main(int argc, char** argv) {
    int  port     = 5555;
    bool rtt_mode = false;
    bool udp_mode = false; // false=TCP, true=UDP

    // 인자 해석:
    //  - ./ubuntu_tcp_server                     -> TCP:5555 (기본)
    //  - ./ubuntu_tcp_server 6000                -> TCP:6000
    //  - ./ubuntu_tcp_server rtt [port]          -> TCP RTT
    //  - ./ubuntu_tcp_server udp [port]          -> UDP:5555
    //  - ./ubuntu_tcp_server udp rtt [port]      -> UDP RTT
    //  - ./ubuntu_tcp_server tcp rtt [port]      -> TCP RTT (명시)
    if (argc >= 2)
    {
        std::string arg1 = argv[1];
        int i = 1;

        if (arg1 == "udp")
        {
            udp_mode = true;
            ++i;
        }
        else if (arg1 == "tcp")
        {
            udp_mode = false;
            ++i;
        }

        if (i < argc)
        {
            std::string a = argv[i];
            if (a == "rtt")
            {
                rtt_mode = true;
                ++i;
                if (i < argc)
                    port = std::stoi(argv[i]);
            }
            else
            {
                // 숫자면 port로 해석
                try {
                    port = std::stoi(a);
                } catch (...) {
                    // ignore
                }
            }
        }
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    int server_fd = ::socket(AF_INET, udp_mode ? SOCK_DGRAM : SOCK_STREAM, 0);
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

    std::cout << "[TCP] Ubuntu TCP 서버 시작" << std::endl;
    std::cout << "[TCP] Listening on 0.0.0.0:" << port << std::endl;
    if (!udp_mode)
    {
        std::cout << "[TCP] ESP8266/라즈베리는 <서버IP>:" << port
                  << " 로 접속하면 됨" << std::endl;
    }
    else
    {
        std::cout << "[UDP] 모드: UDP " << port << " (라즈베리가 먼저 HELLO/아무 패킷을 보내 peer를 알려줘야 함)\n";
    }
    if (rtt_mode) {
        std::cout << "[TCP] 모드: RTT 측정 (클라이언트가 PING 라인을 그대로 echo 하도록 구현 필요)" << std::endl;
    } else {
        std::cout << "[TCP] 모드: 수동 전송 (좌표/문자열을 직접 입력해서 ESP로 전송)" << std::endl;
    }

    // UDP 모드: accept/listen이 없으므로 여기서 분기 처리
    if (udp_mode)
    {
        // UDP bind 완료. peer 학습 + (필요 시) 수신 스레드 시작 후 stdin 라인을 peer로 sendto
        UdpPeer peer;
        std::atomic<bool> stop_recv{false};
        std::thread recv_th;

        if (!rtt_mode)
        {
            recv_th = std::thread(udp_recv_thread_fn, server_fd, std::ref(stop_recv), std::ref(peer));
        }

        if (rtt_mode)
        {
            run_udp_rtt_test(server_fd, peer);
        }
        else
        {
            std::cout << "[UDP] 파이프 수신 대기 중 (rtsp_laser_demo | ubuntu_tcp_server udp ...)\n";
            std::string line;
            while (g_running && std::getline(std::cin, line))
            {
                if (line == "quit" || line == "exit") break;
                if (line.empty()) continue;

                // peer가 아직 없으면 기다림 (라즈베리가 HELLO 필요)
                if (!peer.valid)
                    continue;

                std::string msg = line;
                // stdin 라인은 개행이 제거되어 있으므로 \n 추가
                msg.push_back('\n');
                if (::sendto(server_fd, msg.c_str(), static_cast<int>(msg.size()), 0,
                             reinterpret_cast<const sockaddr*>(&peer.addr), peer.addrlen) <= 0)
                {
                    std::cerr << "[UDP] sendto 실패: " << std::strerror(errno) << "\n";
                    continue;
                }
            }
        }

        stop_recv.store(true);
        if (recv_th.joinable()) recv_th.join();
        ::close(server_fd);
        std::cout << "[UDP] 서버 종료" << std::endl;
        return 0;
    }

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

        // rtsp_laser_demo에 "연결됨" 신호 전달 → 레이저 탐지/LUT 시작
        ::mkfifo(LUT_CLIENT_CONNECTED_FIFO, 0666);
        int conn_fd = ::open(LUT_CLIENT_CONNECTED_FIFO, O_WRONLY);
        if (conn_fd >= 0) {
            const char* msg = "CONNECTED\n";
            ssize_t wr = ::write(conn_fd, msg, std::strlen(msg));
            (void)wr;
            ::close(conn_fd);
        }

        // TCP 수신 스레드 시작 (PAN/TILT 라인 수신)
        std::atomic<bool> stop_recv{false};
        std::thread recv_th(recv_thread_fn, client_fd, std::ref(stop_recv));

        if (rtt_mode) {
            run_rtt_test(client_fd);
        } else {
            std::cout << "[TCP] 파이프 수신 대기 중 (rtsp_laser_demo | ubuntu_tcp_server)" << std::endl;

            std::string line;
            while (g_running && std::getline(std::cin, line)) {
                if (line == "quit" || line == "exit") break;

                std::string trimmed = line;
                while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == '\n' || trimmed.back() == ' '))
                    trimmed.pop_back();
                while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
                    trimmed.erase(trimmed.begin());

                if (trimmed.empty())
                    continue;

                // raw forward: REQUEST_PWM / NEXT_GRID / RESET_HOME / SET_PWM
                if (trimmed.find("REQUEST_PWM,") == 0 ||
                    trimmed.find("NEXT_GRID,") == 0 ||
                    trimmed == "RESET_HOME" || trimmed.find("RESET_HOME,") == 0 ||
                    trimmed.find("SET_PWM,") == 0)
                {
                    std::string msg = trimmed + "\n";
                    if (::send(client_fd, msg.c_str(), static_cast<int>(msg.size()), MSG_NOSIGNAL) <= 0) {
                        std::cerr << "[TCP] 클라이언트 연결 끊김, 재접속 대기...\n";

                        stop_recv.store(true);
                        if (recv_th.joinable()) recv_th.join();
                        ::close(client_fd);

                        // 새 클라이언트 접속까지 대기
                        client_fd = -1;
                        while (g_running && client_fd < 0) {
                            client_fd = ::accept(server_fd,
                                reinterpret_cast<sockaddr*>(&client_addr), &client_len);
                            if (client_fd < 0) {
                                if (!g_running) break;
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            }
                        }
                        if (client_fd < 0) break;

                        ::inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
                        std::cout << "[TCP] 재접속: " << client_ip
                                  << ":" << ntohs(client_addr.sin_port) << std::endl;

                        // 수신 스레드 재시작
                        stop_recv.store(false);
                        recv_th = std::thread(recv_thread_fn, client_fd, std::ref(stop_recv));

                        // 재전송
                        if (::send(client_fd, msg.c_str(), static_cast<int>(msg.size()), MSG_NOSIGNAL) > 0)
                            std::cout << "[TCP] Sent (재전송): " << msg;
                        continue;
                    }
                    std::cout << "[TCP] Sent: " << msg;
                    continue;
                }

                float e_x = 0.0f, e_y = 0.0f;
                float t_u = 0.0f, t_v = 0.0f;
                int   g_r = -1,   g_c = -1;

                int n = std::sscanf(trimmed.c_str(), "%f %f %f %f %d %d",
                                    &e_x, &e_y, &t_u, &t_v, &g_r, &g_c);

                char send_buf[128];
                int len = 0;

                if (n >= 6) {
                    len = std::snprintf(send_buf, sizeof(send_buf),
                        "EX=%.6f,EY=%.6f,TU=%.3f,TV=%.3f,GR=%d,GC=%d\n",
                        e_x, e_y, t_u, t_v, g_r, g_c);
                } else if (n >= 4) {
                    len = std::snprintf(send_buf, sizeof(send_buf),
                        "EX=%.6f,EY=%.6f,TU=%.3f,TV=%.3f\n",
                        e_x, e_y, t_u, t_v);
                } else if (n >= 2) {
                    len = std::snprintf(send_buf, sizeof(send_buf),
                        "EX=%.6f,EY=%.6f\n", e_x, e_y);
                } else {
                    continue;
                }

                if (len <= 0 || len >= static_cast<int>(sizeof(send_buf)))
                    continue;

                // send 실패 → 연결 끊김, 재접속 대기
                if (::send(client_fd, send_buf, len, MSG_NOSIGNAL) <= 0) {
                    std::cerr << "[TCP] 클라이언트 연결 끊김, 재접속 대기...\n";

                    stop_recv.store(true);
                    if (recv_th.joinable()) recv_th.join();
                    ::close(client_fd);

                    // 새 클라이언트 접속까지 대기
                    client_fd = -1;
                    while (g_running && client_fd < 0) {
                        client_fd = ::accept(server_fd,
                            reinterpret_cast<sockaddr*>(&client_addr), &client_len);
                        if (client_fd < 0) {
                            if (!g_running) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                    }
                    if (client_fd < 0) break;

                    ::inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
                    std::cout << "[TCP] 재접속: " << client_ip
                              << ":" << ntohs(client_addr.sin_port) << std::endl;

                    // 수신 스레드 재시작
                    stop_recv.store(false);
                    recv_th = std::thread(recv_thread_fn, client_fd, std::ref(stop_recv));

                    // 방금 실패한 메시지 재전송
                    if (::send(client_fd, send_buf, len, MSG_NOSIGNAL) > 0)
                        std::cout << "[TCP] Sent (재전송): " << send_buf;
                    continue;
                }
                std::cout << "[TCP] Sent: " << send_buf;
            }

            if (!std::cin.good())
                g_running = false;
        }

        stop_recv.store(true);
        if (recv_th.joinable()) recv_th.join();

        ::close(client_fd);
        if (!g_running || rtt_mode) break; // RTT 모드는 1회 측정 후 종료
    }

    ::close(server_fd);
    std::cout << "[TCP] 서버 종료" << std::endl;
    return 0;
}

