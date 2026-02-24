#include <iostream>
#include <filesystem>
#include <thread>
#include <atomic>
#include <chrono>
#include <csignal> // 시그널 처리를 위해 필요
#include <arpa/inet.h>
#include <unistd.h>
#include <fstream>
#include <vector>
#include <mutex>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <iomanip>
#include <cstdint>

#include "log.h"
#include "recorder.h"
#include "cleanup.h"
#include "auth.h"
#include "rfid_monitor.h"

#include "audio_common.h"
#include "audio_ring_buffer.h"
#include "audio_playback.h"
#include "analytics.h"

namespace fs = std::filesystem;

// [설정] 로그인 인증 전용 포트 및 DB 접속 정보
#define AUTH_PORT 5555           // Qt 클라이언트와 통신할 포트
#define AUDIO_PORT 5556          // 음성 데이터 수신 포트
#define VOICE_SAVE_DIR "voice_recs"
#define DB_HOST "192.168.0.92"   // MariaDB 서버 IP
#define DB_USER "pi"             // DB 사용자 아이디
#define DB_PASS "raspberry"      // DB 비밀번호
#define DB_NAME "Client_db"      // 사용할 데이터베이스 이름
#define ALERT_PORT 5557          // 부정승차 알림 포트

// 알림 전송용 클라이언트 소켓 관리
std::vector<int> g_client_sockets;
std::mutex g_sockets_mutex;
std::atomic<bool> g_running(true);

void close_alert_client_sockets() {
    std::lock_guard<std::mutex> lock(g_sockets_mutex);
    for (int fd : g_client_sockets) {
        close(fd);
    }
    g_client_sockets.clear();
}

// 데이터를 콜백 함수로 넘기기 위한 구조체
struct ServerData {
    DBLogger *logger;
};

// 음성 수신 스레드: TCP로 RAW PCM 수신 → 링 버퍼 → ALSA 재생 스레드 (Audio_Speaker_Unit 방식)
void run_audio_receiver() {
    const std::size_t RING_CAPACITY_BYTES = AUDIO_SAMPLE_RATE * AUDIO_FRAME_BYTES * 1;  // 약 1초 분량
    AudioRingBuffer ring(RING_CAPACITY_BYTES);
    AudioPlayback playback(ring);

    if (!playback.start()) {
        std::cerr << "[Audio] Failed to start AudioPlayback" << std::endl;
        return;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::perror("[Audio] socket");
        playback.stop();
        return;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(AUDIO_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        std::perror("[Audio] bind");
        close(server_fd);
        playback.stop();
        return;
    }
    if (listen(server_fd, 5) < 0) {
        std::perror("[Audio] listen");
        close(server_fd);
        playback.stop();
        return;
    }

    std::cout << "[Audio] RAW mode (16kHz, mono, S16_LE) on port " << AUDIO_PORT << " ..." << std::endl;

    constexpr std::size_t BUF_SIZE = 4096;
    char buf[BUF_SIZE];

    while (g_running) {
        struct pollfd pfd = {server_fd, POLLIN, 0};
        int poll_ret = poll(&pfd, 1, 1000);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::perror("[Audio] poll");
            break;
        }
        if (poll_ret == 0) continue;

        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (!g_running) break;
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            std::perror("[Audio] accept");
            continue;
        }

        std::cout << "[Audio] Client connected." << std::endl;
        std::size_t total_bytes = 0;
        std::uint64_t sample_count = 0;
        std::uint64_t abs_sum = 0;
        int peak_abs = 0;
        const auto conn_start = std::chrono::steady_clock::now();

        ssize_t bytes;
        while (g_running) {
            struct pollfd cfd = {client_fd, POLLIN, 0};
            int c_poll = poll(&cfd, 1, 1000);
            if (c_poll < 0) {
                if (errno == EINTR) continue;
                std::perror("[Audio] client poll");
                break;
            }
            if (c_poll == 0) continue;

            bytes = read(client_fd, buf, BUF_SIZE);
            if (bytes > 0) {
                ring.push(buf, static_cast<std::size_t>(bytes));
                total_bytes += static_cast<std::size_t>(bytes);

                const std::size_t sample_bytes = static_cast<std::size_t>(bytes) - (static_cast<std::size_t>(bytes) % sizeof(std::int16_t));
                const auto* samples = reinterpret_cast<const std::int16_t*>(buf);
                const std::size_t n = sample_bytes / sizeof(std::int16_t);
                for (std::size_t i = 0; i < n; ++i) {
                    int v = static_cast<int>(samples[i]);
                    int a = (v < 0) ? -v : v;
                    abs_sum += static_cast<std::uint64_t>(a);
                    if (a > peak_abs) peak_abs = a;
                }
                sample_count += n;
                continue;
            }
            if (bytes == 0) break;
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            std::perror("[Audio] read");
            break;
        }

        close(client_fd);
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - conn_start).count();
        const double pcm_seconds = static_cast<double>(total_bytes) / static_cast<double>(AUDIO_SAMPLE_RATE * AUDIO_FRAME_BYTES);
        const double avg_abs = (sample_count > 0) ? (static_cast<double>(abs_sum) / static_cast<double>(sample_count)) : 0.0;
        std::cout << "[Audio] Client disconnected. bytes=" << total_bytes
                  << ", conn_ms=" << elapsed_ms
                  << ", pcm_sec=" << std::fixed << std::setprecision(2) << pcm_seconds
                  << ", avg_abs=" << std::fixed << std::setprecision(1) << avg_abs
                  << ", peak_abs=" << peak_abs << std::endl;
        if (total_bytes == 0) {
            std::cerr << "[Audio] Warning: connection closed without PCM payload." << std::endl;
        } else if (sample_count > 0 && peak_abs < 50) {
            std::cerr << "[Audio] Warning: payload looks near-silent (very low peak)." << std::endl;
        }
    }

    ring.stop();
    playback.stop();
    close(server_fd);
}

// 부정승차 알림 서버 (클라이언트 연결 관리)
void run_fraud_notifier() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "[Alert] socket() failed: " << strerror(errno) << std::endl;
        return;
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {AF_INET, htons(ALERT_PORT), {INADDR_ANY}};
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        std::cerr << "[Alert] bind() failed: " << strerror(errno) << std::endl;
        close(server_fd);
        return;
    }

    if (listen(server_fd, 5) < 0) {
        std::cerr << "[Alert] listen() failed: " << strerror(errno) << std::endl;
        close(server_fd);
        return;
    }

    std::cout << "[Alert] Alert server listening on port " << ALERT_PORT << "..." << std::endl;

    while (g_running) {
        struct pollfd pfd = {server_fd, POLLIN, 0};
        int poll_ret = poll(&pfd, 1, 1000);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Alert] poll() failed: " << strerror(errno) << std::endl;
            break;
        }
        if (poll_ret == 0) continue;

        sockaddr_in peer_addr {};
        socklen_t peer_len = sizeof(peer_addr);
        int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
        if (client_fd >= 0) {
            std::lock_guard<std::mutex> lock(g_sockets_mutex);
            g_client_sockets.push_back(client_fd);
            std::cout << "[Alert] Client connected for fraud notifications: "
                      << inet_ntoa(peer_addr.sin_addr) << ":" << ntohs(peer_addr.sin_port) << " (fd=" << client_fd << ")"
                      << std::endl;
        } else {
            if (!g_running) break;
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            std::cerr << "[Alert] accept() failed: " << strerror(errno) << std::endl;
        }
    }

    close(server_fd);
    close_alert_client_sockets();
    std::cout << "[Alert] notifier thread stopped." << std::endl;
}

// 더미 부정승차 데이터 생성기
void run_dummy_fraud_generator() {
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(5)); // 5초마다 발생

        // 1. 더미 데이터 생성
        std::string cardId = "CARD_" + std::to_string(rand() % 9000 + 1000);
        std::string ageGroup = (rand() % 2 == 0) ? "Senior" : "Youth";
        int gateId = rand() % 5 + 1;
        int estAge = rand() % 40 + 15; // 15~55세

        // 2. 클라이언트에 전송 (형식: "FRAUD|CardID|AgeGroup|Gate|EstAge")
        std::string msg = "FRAUD|" + cardId + "|" + ageGroup + "|" + std::to_string(gateId) + "|" + std::to_string(estAge) + "\n";
        
        std::lock_guard<std::mutex> lock(g_sockets_mutex);
        for (auto it = g_client_sockets.begin(); it != g_client_sockets.end(); ) {
            if (send(*it, msg.c_str(), msg.length(), 0) <= 0) {
                close(*it);
                it = g_client_sockets.erase(it);
            } else {
                ++it;
            }
        }
        std::cout << "[Alert] Fraud detected and broadcasted (5s interval): " << cardId << std::endl;
    }
}

// 로그인 인증 전용 스레드 함수
void run_login_auth() {
    DBLogger db(DB_NAME); // 로그 기록용 객체
    Authenticator auth(DB_HOST, DB_USER, DB_PASS, DB_NAME); // ID/PW 검증용 객체
    
    // DB 연결 확인 (로그용, 인증용 각각 연결)
    const bool db_ok = db.connect();
    const bool auth_ok = auth.connect();
    const bool auth_bypass_mode = !(db_ok && auth_ok);
    if (auth_bypass_mode) {
        std::cerr << "[Warn] Auth DB unavailable. Temporary auth bypass mode enabled (all login requests PASS)." << std::endl;
    }

    // TCP 소켓 서버 설정
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "[Auth] socket() failed: " << strerror(errno) << std::endl;
        return;
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 주소 및 포트 바인딩 (간결한 구조체 초기화 방식 사용)
    struct sockaddr_in addr = {AF_INET, htons(AUTH_PORT), {INADDR_ANY}};
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        std::cerr << "[Auth] bind() failed: " << strerror(errno) << std::endl;
        close(server_fd);
        return;
    }
    if (listen(server_fd, 5) < 0) { // 최대 5개 대기열
        std::cerr << "[Auth] listen() failed: " << strerror(errno) << std::endl;
        close(server_fd);
        return;
    }

    while (g_running) {
        struct pollfd pfd = {server_fd, POLLIN, 0};
        int poll_ret = poll(&pfd, 1, 1000);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Auth] poll() failed: " << strerror(errno) << std::endl;
            break;
        }
        if (poll_ret == 0) continue;

        // 클라이언트 접속 대기
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (!g_running) break;
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            std::cerr << "[Auth] accept() failed: " << strerror(errno) << std::endl;
            continue;
        }

        char buf[1024] = {0};

        // 데이터 수신 ("ID:PW" 형식 예상)
        struct pollfd cfd = {client_fd, POLLIN, 0};
        int c_poll = poll(&cfd, 1, 1000);
        if (c_poll > 0 && read(client_fd, buf, sizeof(buf)) > 0) {
            std::string data(buf), user = "Unknown";
            size_t sep = data.find(':');
            bool success = auth_bypass_mode;

            // 구분자(:)가 있을 경우에만 분석 진행
            if (sep != std::string::npos) {
                user = data.substr(0, sep);
                std::string pass = data.substr(sep + 1);
                
                // 불필요한 공백/개행 제거
                user.erase(user.find_last_not_of(" \n\r\t") + 1);
                pass.erase(pass.find_last_not_of(" \n\r\t") + 1);
                
                if (!auth_bypass_mode) {
                    success = auth.authenticate(user, pass);
                }
            }  
              
            // 검증 결과 전송
            send(client_fd, success ? "PASS" : "FAIL", 4, 0);

            // [로그 기록] 새로 만든 login_logs 테이블에 기록
            // 사용자의 IP 주소를 가져오기 위해 sockaddr_in 정보를 같이 활용할 수도 있으나,
            // 현재는 구조상 간단하게 유저 정보와 성공여부만 기록합니다. (IP는 로그 클래스 내부 처리 유도)
            db.enqueueLogin(user, "Unknown_IP", success);
        }
        close(client_fd); // 세션 종료
    }

    close(server_fd);
    std::cout << "[Auth] auth thread stopped." << std::endl;
}

// [핵심] Ctrl+C 감지 함수
void signal_handler(int signum) {
    (void)signum;
    g_running = false; // 루프를 멈추게 함 -> 자연스럽게 저장 로직 실행됨
}


int main() {
    // 1. 종료 신호(SIGINT) 등록
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 2. 디렉토리 생성
    if (!fs::exists(VIDEO_SAVE_DIR)) fs::create_directories(VIDEO_SAVE_DIR);

    // 3. DB 연결 (logger) 및 AnalyticsProcessor 시작
    DBLogger logger;
    const bool logger_connected = logger.connect();
    if (!logger_connected) {
        std::cerr << "[Warn] DB Connection failed. Continuing without DB logging." << std::endl;
    }

    // AnalyticsProcessor: analytics_logs는 CCgbd DB에 있음
    AnalyticsProcessor analytics(DB_HOST, DB_USER, DB_PASS, "CCgbd", 3840, 2160);
    if (!analytics.start()) {
        std::cerr << "[Warn] Analytics processor failed to start. Continuing without analytics worker." << std::endl;
    }

    // 4. 파일 정리 스레드 시작
    // 전역 변수 g_running을 참조로 넘김
    std::thread t1(run_file_cleanup_worker, std::ref(g_running), std::string(VIDEO_SAVE_DIR), 300);

    // 5. DB 정리 스레드 시작
    std::thread t2;
    if (logger_connected) {
        t2 = std::thread([&](){ 
            while(g_running) { 
                std::this_thread::sleep_for(std::chrono::seconds(60)); 
                logger.requestDbCleanup(); 
            } 
        });
    } else {
        std::cout << "[System] DB cleanup worker skipped (DB unavailable)." << std::endl;
    }

    // 6. 로그인 인증 스레드 시작
    std::thread t3(run_login_auth);

    // 7. 음성 수신 스레드 시작
    std::thread t4(run_audio_receiver);

    // 8. 녹화 시작
    // [NEW] 9. RFID 모니터링 스레드 시작 (recorder.run() 이전에 시작)
    RfidMonitor rfid_monitor(g_running, DB_HOST, DB_USER, DB_PASS, DB_NAME);
    std::thread t5(&RfidMonitor::start, &rfid_monitor);

    std::cout << "[System] RFID 모니터링 서비스 시작됨." << std::endl;

    

    // 8. 부정승차 알림 서버 시작
    std::thread t6(run_fraud_notifier);

    // // 9. 더미 부정승차 생성기 시작
    // std::thread t7(run_dummy_fraud_generator);
    // t7.detach();

    // 10. 녹화 시작
    RTSPRecorder recorder(logger, g_running, analytics);
    recorder.run(); // 메인 스레드 블로킹

    // recorder 루프가 끝나면 나머지 스레드도 종료 신호 전달
    g_running = false;

    if (t6.joinable()) t6.join();
    if (t5.joinable()) t5.join();
    if (t4.joinable()) t4.join();
    if (t3.joinable()) t3.join();
    if (t2.joinable()) t2.join();
    if (t1.joinable()) t1.join();

    std::cout << "[System] 서버가 안전하게 종료되었습니다." << std::endl;
    return 0;
}
