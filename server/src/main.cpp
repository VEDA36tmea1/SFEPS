#include <iostream>
#include <filesystem>
#include <thread>
#include <atomic>
#include <csignal> // 시그널 처리를 위해 필요
#include <arpa/inet.h>
#include <unistd.h>
#include <fstream>
#include <vector>
#include <cstdio>

#include "log.h"
#include "recorder.h"
#include "cleanup.h"
#include "auth.h"
#include "rfid_monitor.h"

#include "audio_common.h"
#include "audio_ring_buffer.h"
#include "audio_playback.h"

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
void run_fraud_notifier() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {AF_INET, htons(ALERT_PORT), {INADDR_ANY}};
    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5);

    while (true) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd >= 0) {
            std::lock_guard<std::mutex> lock(g_sockets_mutex);
            g_client_sockets.push_back(client_fd);
            std::cout << "[Alert] Client connected for fraud notifications." << std::endl;
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
        std::cout << "[Alert] Fraud detected and broadcasted (5s interval): " << cardId << std::endl;
    }
}

// 로그인 인증 전용 스레드 함수
void run_login_auth() {
    DBLogger db(DB_NAME); // 로그 기록용 객체
    Authenticator auth(DB_HOST, DB_USER, DB_PASS, DB_NAME); // ID/PW 검증용 객체
    
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
        int client_fd = accept(server_fd, NULL, NULL);
        char buf[1024] = {0};

        // 데이터 수신 ("ID:PW" 형식 예상)
        if (read(client_fd, buf, sizeof(buf)) > 0) {
            std::string data(buf), user = "Unknown";
            size_t sep = data.find(':');
            bool success = false;

            // 구분자(:)가 있을 경우에만 분석 진행
            if (sep != std::string::npos) {
                user = data.substr(0, sep);
                std::string pass = data.substr(sep + 1);
                
                // 불필요한 공백/개행 제거
                user.erase(user.find_last_not_of(" \n\r\t") + 1);
                pass.erase(pass.find_last_not_of(" \n\r\t") + 1);
                
                success = auth.authenticate(user, pass);
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
}

// 전역 플래그 (시그널 핸들러에서 접근하기 위해)
std::atomic<bool> g_running(true);

// [핵심] Ctrl+C 감지 함수
void signal_handler(int signum) {
    std::cout << "\n[System] 종료 신호 감지! 녹화를 저장하고 종료합니다...\n";
    g_running = false; // 루프를 멈추게 함 -> 자연스럽게 저장 로직 실행됨
}


int main() {
    // 1. 종료 신호(SIGINT) 등록
    signal(SIGINT, signal_handler);

    // 2. 디렉토리 생성
    if (!fs::exists(VIDEO_SAVE_DIR)) fs::create_directories(VIDEO_SAVE_DIR);

    // 3. DB 연결
    DBLogger logger;
    if (!logger.connect()) {
        std::cerr << "[Fatal] DB Connection failed." << std::endl;
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
    std::thread t3(run_login_auth);
    t3.detach();

    // 7. 음성 수신 스레드 시작
    std::thread t4(run_audio_receiver);
    t4.detach();

    // 8. 녹화 시작
    // [NEW] 9. RFID 모니터링 스레드 시작 (recorder.run() 이전에 시작)
    RfidMonitor rfid_monitor(g_running, DB_HOST, DB_USER, DB_PASS, DB_NAME);
    std::thread t5(&RfidMonitor::start, &rfid_monitor);
    t5.detach();

    std::cout << "[System] RFID 모니터링 서비스 시작됨." << std::endl;

    

    // 8. 부정승차 알림 서버 시작
    std::thread t6(run_fraud_notifier);
    t6.detach();

    // 9. 더미 부정승차 생성기 시작
    std::thread t7(run_dummy_fraud_generator);
    t7.detach();

    // 10. 녹화 시작
    RTSPRecorder recorder(logger, g_running);
    recorder.run(); // 메인 스레드 블로킹
  
    // detach된 스레드들이 정리될 시간을 약간 줄 수 있음
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "[System] 서버가 안전하게 종료되었습니다." << std::endl;
    return 0;
}