#include <iostream>
#include <filesystem>
#include <thread>
#include <atomic>
#include <csignal> // 시그널 처리를 위해 필요
#include <chrono>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
#include <cstdio>

#include "log.h"
#include "recorder.h" 
#include "cleanup.h" 
#include "auth.h"
#include "rfid_monitor.h" // [NEW] RFID 모니터링 헤더 추가
#include "analytics.h"
#include "alert.h"
#include "runtime_config.h"

namespace fs = std::filesystem;

// [설정] 포트 정보
#define AUTH_PORT 5555           // Qt 클라이언트와 통신할 포트
#define AUDIO_PORT 5556          // 음성 데이터 수신 포트
#define ALERT_PORT 5557          // 부정승차 알림 포트

// 알림 전송용 클라이언트 소켓 관리
std::vector<int> g_client_sockets;
std::mutex g_sockets_mutex;

// 음성 수신 스레드 함수
void run_audio_receiver() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {AF_INET, htons(AUDIO_PORT), {INADDR_ANY}};
    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5);

    while (true) {
        int client_fd = accept(server_fd, NULL, NULL);
        
        // 메모리 버퍼에 오디오 데이터 수집
        std::vector<char> audio_buffer;
        char buf[4096];
        ssize_t bytes;
        while ((bytes = read(client_fd, buf, sizeof(buf))) > 0) {
            audio_buffer.insert(audio_buffer.end(), buf, buf + bytes);
        }
        close(client_fd);

        if (audio_buffer.empty()) {
            std::cout << "[main.cpp] " << "[Audio] Received empty data, skipping playback" << std::endl;
            continue;
        }

        std::cout << "[main.cpp] " << "[Audio] Received " << audio_buffer.size() << " bytes, playing..." << std::endl;

        // ALSA로 바로 재생 (16000 Hz, 모노, S16_LE)
        FILE* aplay = popen("aplay -f S16_LE -r 16000 -c 1 -D default", "w");
        if (aplay) {
            size_t written = fwrite(audio_buffer.data(), 1, audio_buffer.size(), aplay);
            pclose(aplay);
            if (written == audio_buffer.size()) {
                std::cout << "[main.cpp] " << "[Audio] Playback completed" << std::endl;
            } else {
                std::cerr << "[Audio] Playback error: wrote " << written << " / " << audio_buffer.size() << " bytes" << std::endl;
            }
        } else {
            std::cerr << "[Audio] Failed to start aplay" << std::endl;
        }
    }
}

// 부정승차 알림 서버 (클라이언트 연결 관리)
void run_fraud_notifier() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {AF_INET, htons(ALERT_PORT), {INADDR_ANY}};
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        std::cerr << "[Alert] bind() failed: " << strerror(errno) << std::endl;
        return;
    }

    if (listen(server_fd, 5) < 0) {
        std::cerr << "[Alert] listen() failed: " << strerror(errno) << std::endl;
        return;
    }

    std::cout << "[main.cpp] " << "[Alert] Alert server listening on port " << ALERT_PORT << "..." << std::endl;

    while (true) {
        sockaddr_in peer_addr {};
        socklen_t peer_len = sizeof(peer_addr);
        int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
        if (client_fd >= 0) {
            std::lock_guard<std::mutex> lock(g_sockets_mutex);
            g_client_sockets.push_back(client_fd);
            std::cout << "[main.cpp] " << "[Alert] Client connected for fraud notifications: "
                      << inet_ntoa(peer_addr.sin_addr) << ":" << ntohs(peer_addr.sin_port) << " (fd=" << client_fd << ")"
                      << std::endl;
        } else {
            std::cerr << "[Alert] accept() failed: " << strerror(errno) << std::endl;
        }
    }
}

// 더미 부정승차 데이터 생성기
void run_dummy_fraud_generator() {
    while (true) {
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
        std::cout << "[main.cpp] " << "[Alert] Fraud detected and broadcasted (5s interval): " << cardId << std::endl;
    }
}

namespace {
std::string trim_copy(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

std::string normalize_login_key(const std::string& user) {
    std::string normalized = trim_copy(user);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized;
}
} // namespace

// 로그인 인증 전용 스레드 함수
void run_login_auth(const RuntimeConfig cfg) {
    struct AttemptState {
        int fail_count = 0;
        std::chrono::steady_clock::time_point lock_until = std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point last_seen = std::chrono::steady_clock::time_point::min();
    };

    constexpr int kMaxFail = 5;
    constexpr int kCleanupInterval = 100;
    const auto kLockDuration = std::chrono::seconds(30);
    const auto kStaleRetention = std::chrono::minutes(10);

    std::unordered_map<std::string, AttemptState> attempts;
    int request_counter = 0;

    DBLogger db(cfg.db_host.c_str(), cfg.db_user.c_str(), cfg.db_pass.c_str(), cfg.db_name_auth.c_str()); // 로그 기록용 객체
    Authenticator auth(cfg.db_host.c_str(), cfg.db_user.c_str(), cfg.db_pass.c_str(), cfg.db_name_auth.c_str()); // ID/PW 검증용 객체
    
    // DB 연결 확인 (로그용, 인증용 각각 연결)
    if (!db.connect() || !auth.connect()) {
        std::cerr << "[Fatal] Auth-related DB connection failed." << std::endl;
        return;
    }

    // TCP 소켓 서버 설정
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 주소 및 포트 바인딩 (간결한 구조체 초기화 방식 사용)
    struct sockaddr_in addr = {AF_INET, htons(AUTH_PORT), {INADDR_ANY}};
    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5); // 최대 5개 대기열

    while (true) {
        // 클라이언트 접속 대기
        sockaddr_in peer_addr {};
        socklen_t peer_len = sizeof(peer_addr);
        int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
        if (client_fd < 0) continue;

        char ip_buf[INET_ADDRSTRLEN] = {0};
        const char* ip_res = inet_ntop(AF_INET, &peer_addr.sin_addr, ip_buf, sizeof(ip_buf));
        std::string client_ip = (ip_res != NULL) ? std::string(ip_res) : std::string("Unknown_IP");

        char buf[1024] = {0};

        // 데이터 수신 ("ID:PW" 형식 예상)
        if (read(client_fd, buf, sizeof(buf)) > 0) {
            std::string data(buf), user = "Unknown";
            size_t sep = data.find(':');
            bool success = false;
            bool valid_format = false;

            // 구분자(:)가 있을 경우에만 분석 진행
            if (sep != std::string::npos) {
                user = trim_copy(data.substr(0, sep));
                std::string pass = trim_copy(data.substr(sep + 1));
                if (!user.empty() && !pass.empty()) {
                    valid_format = true;
                    const std::string login_key = normalize_login_key(user) + "|" + client_ip;
                    auto now = std::chrono::steady_clock::now();
                    AttemptState& state = attempts[login_key];
                    state.last_seen = now;

                    if (state.lock_until > now) {
                        success = false;
                        std::cout << "[main.cpp] " << "[Auth] locked user blocked: "
                                  << user << " ip=" << client_ip << std::endl;
                    } else {
                        success = auth.authenticate(user, pass);
                        if (success) {
                            state.fail_count = 0;
                            state.lock_until = std::chrono::steady_clock::time_point::min();
                        } else {
                            state.fail_count += 1;
                            if (state.fail_count >= kMaxFail) {
                                state.fail_count = 0;
                                state.lock_until = now + kLockDuration;
                                std::cout << "[main.cpp] " << "[Auth] lockout triggered: user="
                                          << user << " ip=" << client_ip << " duration=30s" << std::endl;
                            }
                        }
                    }
                }
            }  

            if (!valid_format) {
                success = false;
            }
              
            // 검증 결과 전송
            send(client_fd, success ? "PASS" : "FAIL", 4, 0);
            if (success) {
                const std::string msg = "TEST|LOGIN_OK|" + user + "\n";
                send_alert_to_clients(msg);
                std::cout << "[main.cpp] " << "[Auth] login success ping sent: " << msg << std::endl;
            }

            // [로그 기록] 새로 만든 login_logs 테이블에 기록
            // 사용자의 IP 주소를 가져오기 위해 sockaddr_in 정보를 같이 활용할 수도 있으나,
            // 현재는 구조상 간단하게 유저 정보와 성공여부만 기록합니다. (IP는 로그 클래스 내부 처리 유도)
            db.enqueueLogin(user, client_ip, success);

            if (++request_counter % kCleanupInterval == 0) {
                auto now = std::chrono::steady_clock::now();
                for (auto it = attempts.begin(); it != attempts.end();) {
                    const bool is_locked = it->second.lock_until > now;
                    const bool is_stale = (it->second.last_seen != std::chrono::steady_clock::time_point::min()) &&
                                          ((now - it->second.last_seen) > kStaleRetention);
                    if (!is_locked && it->second.fail_count == 0 && is_stale) {
                        it = attempts.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }
        close(client_fd); // 세션 종료
    }
}

// 전역 플래그 (시그널 핸들러에서 접근하기 위해)
std::atomic<bool> g_running(true);

// [핵심] Ctrl+C 감지 함수
void signal_handler(int signum) {
    std::cout << "[main.cpp] " << "\n[System] 종료 신호 감지! 녹화를 저장하고 종료합니다...\n";
    g_running = false; // 루프를 멈추게 함 -> 자연스럽게 저장 로직 실행됨
}

//1회테스트
int main(int argc, char* argv[]) {
    RuntimeConfig cfg;
    std::string cfg_err;
    if (!load_runtime_config(cfg, cfg_err)) {
        std::cerr << "[Fatal] Runtime config error: " << cfg_err << std::endl;
        return -1;
    }

    bool send_test_ping = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--ping-2s" || arg == "--test-ping") {
            send_test_ping = true;
        }
    }


    // 1. 종료 신호(SIGINT) 등록
    signal(SIGINT, signal_handler);

    // 2. 디렉토리 생성
    if (!fs::exists(VIDEO_SAVE_DIR)) fs::create_directories(VIDEO_SAVE_DIR);

    // 3. DB 연결 (logger) 및 AnalyticsProcessor 시작
    DBLogger logger(cfg.db_host.c_str(), cfg.db_user.c_str(), cfg.db_pass.c_str(), cfg.db_name_analytics.c_str());
    if (!logger.connect()) {
        std::cerr << "[Fatal] DB Connection failed." << std::endl;
        return -1;
    }

    // AnalyticsProcessor: analytics_logs는 CCgbd DB에 있음
    AnalyticsProcessor analytics(cfg.db_host.c_str(), cfg.db_user.c_str(), cfg.db_pass.c_str(),
                                 cfg.db_name_analytics.c_str(), 3840, 2160);
    if (!analytics.start()) {
        std::cerr << "[Fatal] Analytics DB connection failed." << std::endl;
        return -1;
    }

    // 4. 파일 정리 스레드 시작
    // 전역 변수 g_running을 참조로 넘김
    std::thread t1(run_file_cleanup_worker, std::ref(g_running), std::string(VIDEO_SAVE_DIR), 300);
    t1.detach();

    // 5. DB 정리 스레드 시작
    std::thread t2([&](){ 
        while(g_running) { 
            std::this_thread::sleep_for(std::chrono::seconds(60)); 
            logger.requestDbCleanup(); 
        } 
    });
    t2.detach();

    // 6. 로그인 인증 스레드 시작
    std::thread t3(run_login_auth, cfg);
    t3.detach();

    // 7. 음성 수신 스레드 시작
    std::thread t4(run_audio_receiver);
    t4.detach();

    // 8. 녹화 시작
    // [NEW] 9. RFID 모니터링 스레드 시작 (recorder.run() 이전에 시작)
    RfidMonitor rfid_monitor(g_running, cfg.db_host, cfg.db_user, cfg.db_pass, cfg.db_name_auth);
    std::thread t5(&RfidMonitor::start, &rfid_monitor);
    t5.detach();

    std::cout << "[main.cpp] " << "[System] RFID 모니터링 서비스 시작됨." << std::endl;

    

    // 8. 부정승차 알림 서버 시작
    std::thread t6(run_fraud_notifier);
    t6.detach();

    //1회 신호
    std::thread t7;
    if (send_test_ping) {
        t7 = std::thread([&]() {
            int seq = 0;
            while (g_running) {
                const std::string msg = "TEST|PING|" + std::to_string(seq++);
                send_test_alert_to_clients(msg);
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
            std::cout << "[main.cpp] " << "[Alert] Test ping thread stopped." << std::endl;
        });
        t7.detach();
        std::cout << "[main.cpp] " << "[System] Test ping enabled: send TEST to clients every 2 sec." << std::endl;
    }


    // // 9. 더미 부정승차 생성기 시작
    // std::thread t7(run_dummy_fraud_generator);
    // t7.detach();

    // 10. 녹화 시작
    RTSPRecorder recorder(logger, g_running, analytics);
    recorder.run(); // 메인 스레드 블로킹
  
    // detach된 스레드들이 정리될 시간을 약간 줄 수 있음
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "[main.cpp] " << "[System] 서버가 안전하게 종료되었습니다." << std::endl;
    return 0;
}
