#include <iostream>
#include <filesystem>
#include <thread>
#include <atomic>
#include <csignal> // 시그널 처리를 위해 필요
#include <chrono>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>
#include <cstdio>
#include <limits>
#include <cerrno>

#include "log.h"
#include "recorder.h"
#include "cleanup.h"
#include "auth.h"
#include "rfid_monitor.h"

#include "audio_common.h"
#include "audio_ring_buffer.h"
#include "audio_playback.h"
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

namespace {
struct SecurityRuntimeOptions {
    std::unordered_set<std::string> auth_allow_ips;
    std::unordered_set<std::string> audio_allow_ips;
    std::unordered_set<std::string> alert_allow_ips;
    std::size_t auth_max_bytes = 256;
    std::size_t audio_max_bytes = 4 * 1024 * 1024;
    std::size_t alert_max_clients = 64;
    int socket_read_timeout_ms = 5000;
};

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

std::size_t load_env_size_t(const char* name, std::size_t default_value, std::size_t min_value) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') return default_value;

    errno = 0;
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (errno != 0 || end == raw || (end != nullptr && *end != '\0') || parsed < min_value ||
        parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        std::cerr << "[main.cpp] " << "[Config] Invalid env " << name << "=" << raw
                  << ", using default=" << default_value << std::endl;
        return default_value;
    }
    return static_cast<std::size_t>(parsed);
}

int load_env_int(const char* name, int default_value, int min_value) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') return default_value;

    errno = 0;
    char* end = nullptr;
    long parsed = std::strtol(raw, &end, 10);
    if (errno != 0 || end == raw || (end != nullptr && *end != '\0') || parsed < min_value ||
        parsed > std::numeric_limits<int>::max()) {
        std::cerr << "[main.cpp] " << "[Config] Invalid env " << name << "=" << raw
                  << ", using default=" << default_value << std::endl;
        return default_value;
    }
    return static_cast<int>(parsed);
}

std::unordered_set<std::string> parse_allowlist_env(const char* name) {
    std::unordered_set<std::string> out;
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') return out;

    std::string csv(raw);
    size_t pos = 0;
    while (pos <= csv.size()) {
        size_t comma = csv.find(',', pos);
        std::string token = (comma == std::string::npos) ? csv.substr(pos) : csv.substr(pos, comma - pos);
        token = trim_copy(token);
        if (!token.empty()) out.insert(token);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

SecurityRuntimeOptions load_security_runtime_options() {
    SecurityRuntimeOptions cfg;
    cfg.auth_allow_ips = parse_allowlist_env("SFEPS_AUTH_ALLOW_IPS");
    cfg.audio_allow_ips = parse_allowlist_env("SFEPS_AUDIO_ALLOW_IPS");
    cfg.alert_allow_ips = parse_allowlist_env("SFEPS_ALERT_ALLOW_IPS");
    cfg.auth_max_bytes = load_env_size_t("SFEPS_AUTH_MAX_BYTES", 256, 1);
    cfg.audio_max_bytes = load_env_size_t("SFEPS_AUDIO_MAX_BYTES", 4 * 1024 * 1024, 1024);
    cfg.alert_max_clients = load_env_size_t("SFEPS_ALERT_MAX_CLIENTS", 64, 1);
    cfg.socket_read_timeout_ms = load_env_int("SFEPS_SOCKET_READ_TIMEOUT_MS", 5000, 1);
    return cfg;
}

void log_allowlist_mode(const char* env_name, const std::unordered_set<std::string>& allowlist) {
    if (allowlist.empty()) {
        std::cout << "[main.cpp] " << "[Security] " << env_name
                  << " not set: allow-all mode (compatibility)." << std::endl;
        return;
    }
    std::cout << "[main.cpp] " << "[Security] " << env_name << " enabled with " << allowlist.size()
              << " IP(s)." << std::endl;
}

bool is_ip_allowed(const std::unordered_set<std::string>& allowlist, const std::string& client_ip) {
    if (allowlist.empty()) return true;
    return allowlist.find(client_ip) != allowlist.end();
}

std::string peer_ip_to_string(const sockaddr_in& peer_addr) {
    char ip_buf[INET_ADDRSTRLEN] = {0};
    const char* ip_res = inet_ntop(AF_INET, &peer_addr.sin_addr, ip_buf, sizeof(ip_buf));
    return (ip_res != nullptr) ? std::string(ip_res) : std::string("Unknown_IP");
}

void apply_socket_read_timeout(int fd, int timeout_ms) {
    if (fd < 0 || timeout_ms <= 0) return;
    timeval tv {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        std::cerr << "[main.cpp] " << "[Security] failed to set SO_RCVTIMEO: "
                  << std::strerror(errno) << std::endl;
    }
}
} // namespace

// 음성 수신 스레드 함수
//void run_audio_receiver(const SecurityRuntimeOptions sec_cfg) {

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

    /*while (true) {
        sockaddr_in peer_addr {};
        socklen_t peer_len = sizeof(peer_addr);
        int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
        if (client_fd < 0) continue;

        const std::string client_ip = peer_ip_to_string(peer_addr);
        if (!is_ip_allowed(sec_cfg.audio_allow_ips, client_ip)) {
            std::cout << "[main.cpp] " << "[Audio] Connection rejected by allowlist: ip=" << client_ip << std::endl;
            close(client_fd);
            continue;
        }

        apply_socket_read_timeout(client_fd, sec_cfg.socket_read_timeout_ms);
        
        // 메모리 버퍼에 오디오 데이터 수집
        std::vector<char> audio_buffer;
        bool oversize = false;
        std::size_t total_bytes = 0;
        char buf[4096];
        ssize_t bytes;
        while ((bytes = read(client_fd, buf, sizeof(buf))) > 0) {
            if (total_bytes + static_cast<std::size_t>(bytes) > sec_cfg.audio_max_bytes) {
                oversize = true;
                break;
            }
            audio_buffer.insert(audio_buffer.end(), buf, buf + bytes);
            total_bytes += static_cast<std::size_t>(bytes);
        }
        if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            std::cerr << "[main.cpp] " << "[Audio] read error: " << std::strerror(errno) << std::endl;
        }
        close(client_fd);

        if (oversize) {
            std::cout << "[main.cpp] " << "[Audio] Payload rejected: exceeded SFEPS_AUDIO_MAX_BYTES="
                      << sec_cfg.audio_max_bytes << " (ip=" << client_ip << ")" << std::endl;
            continue;
        }

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
    */
    std::cout << "[Audio] RAW mode (16kHz, mono, S16_LE) on port " << AUDIO_PORT << " ..." << std::endl;

    constexpr std::size_t BUF_SIZE = 4096;
    char buf[BUF_SIZE];

    while (g_running) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (!g_running) break;
            std::perror("[Audio] accept");
            continue;
        }

        std::cout << "[Audio] Client connected." << std::endl;

        ssize_t bytes;
        while (g_running && (bytes = read(client_fd, buf, BUF_SIZE)) > 0) {
            ring.push(buf, static_cast<std::size_t>(bytes));
        }

        close(client_fd);
        std::cout << "[Audio] Client disconnected." << std::endl;
    }

    ring.stop();
    playback.stop();
    close(server_fd);
}

// 부정승차 알림 서버 (클라이언트 연결 관리)
void run_fraud_notifier(const SecurityRuntimeOptions sec_cfg) {
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
            const std::string client_ip = peer_ip_to_string(peer_addr);
            if (!is_ip_allowed(sec_cfg.alert_allow_ips, client_ip)) {
                std::cout << "[main.cpp] " << "[Alert] Connection rejected by allowlist: ip=" << client_ip << std::endl;
                close(client_fd);
                continue;
            }

            apply_socket_read_timeout(client_fd, sec_cfg.socket_read_timeout_ms);

            std::lock_guard<std::mutex> lock(g_sockets_mutex);
            if (g_client_sockets.size() >= sec_cfg.alert_max_clients) {
                std::cout << "[main.cpp] " << "[Alert] Connection rejected: max clients reached ("
                          << sec_cfg.alert_max_clients << ")" << std::endl;
                close(client_fd);
                continue;
            }
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

// 로그인 인증 전용 스레드 함수
void run_login_auth(const RuntimeConfig cfg, const SecurityRuntimeOptions sec_cfg) {
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

        const std::string client_ip = peer_ip_to_string(peer_addr);
        if (!is_ip_allowed(sec_cfg.auth_allow_ips, client_ip)) {
            std::cout << "[main.cpp] " << "[Auth] Connection rejected by allowlist: ip=" << client_ip << std::endl;
            close(client_fd);
            continue;
        }

        apply_socket_read_timeout(client_fd, sec_cfg.socket_read_timeout_ms);

        std::vector<char> buf(sec_cfg.auth_max_bytes + 1, 0);

        // 데이터 수신 ("ID:PW" 형식 예상)
        ssize_t bytes_read = read(client_fd, buf.data(), buf.size());
        if (bytes_read > 0) {
            const bool oversized = static_cast<std::size_t>(bytes_read) > sec_cfg.auth_max_bytes;
            const std::size_t copied_size =
                oversized ? sec_cfg.auth_max_bytes : static_cast<std::size_t>(bytes_read);
            std::string data(buf.data(), copied_size);
            std::string user = "Unknown";
            size_t sep = data.find(':');
            bool success = false;
            bool valid_format = false;

            // 구분자(:)가 있을 경우에만 분석 진행
            if (!oversized && sep != std::string::npos) {
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

            if (oversized) {
                std::cout << "[main.cpp] " << "[Auth] Payload rejected: exceeded SFEPS_AUTH_MAX_BYTES="
                          << sec_cfg.auth_max_bytes << " (ip=" << client_ip << ")" << std::endl;
                success = false;
            } else if (!valid_format) {
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

    const SecurityRuntimeOptions sec_cfg = load_security_runtime_options();
    log_allowlist_mode("SFEPS_AUTH_ALLOW_IPS", sec_cfg.auth_allow_ips);
    log_allowlist_mode("SFEPS_AUDIO_ALLOW_IPS", sec_cfg.audio_allow_ips);
    log_allowlist_mode("SFEPS_ALERT_ALLOW_IPS", sec_cfg.alert_allow_ips);
    std::cout << "[main.cpp] " << "[Security] auth_max_bytes=" << sec_cfg.auth_max_bytes
              << ", audio_max_bytes=" << sec_cfg.audio_max_bytes
              << ", alert_max_clients=" << sec_cfg.alert_max_clients
              << ", socket_read_timeout_ms=" << sec_cfg.socket_read_timeout_ms << std::endl;

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
    std::thread t3(run_login_auth, cfg, sec_cfg);
    t3.detach();

    // 7. 음성 수신 스레드 시작
    std::thread t4(run_audio_receiver, sec_cfg);
    t4.detach();

    // 8. 녹화 시작
    // [NEW] 9. RFID 모니터링 스레드 시작 (recorder.run() 이전에 시작)
    RfidMonitor rfid_monitor(g_running, cfg.db_host, cfg.db_user, cfg.db_pass, cfg.db_name_auth);
    std::thread t5(&RfidMonitor::start, &rfid_monitor);
    t5.detach();

    std::cout << "[main.cpp] " << "[System] RFID 모니터링 서비스 시작됨." << std::endl;

    

    // 8. 부정승차 알림 서버 시작
    std::thread t6(run_fraud_notifier, sec_cfg);
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
