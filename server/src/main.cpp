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
#include "rfid_monitor.h" // [NEW] RFID 모니터링 헤더 추가

namespace fs = std::filesystem;

// [설정] 로그인 인증 전용 포트 및 DB 접속 정보
#define AUTH_PORT 5555           // Qt 클라이언트와 통신할 포트
#define AUDIO_PORT 5556          // 음성 데이터 수신 포트
#define VOICE_SAVE_DIR "voice_recs"
#define DB_HOST "192.168.0.92"   // MariaDB 서버 IP
#define DB_USER "pi"             // DB 사용자 아이디
#define DB_PASS "raspberry"      // DB 비밀번호
#define DB_NAME "Client_db"      // 사용할 데이터베이스 이름

// 데이터를 콜백 함수로 넘기기 위한 구조체
struct ServerData {
    DBLogger *logger;
};

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
            std::cout << "[Audio] Received empty data, skipping playback" << std::endl;
            continue;
        }

        std::cout << "[Audio] Received " << audio_buffer.size() << " bytes, playing..." << std::endl;

        // ALSA로 바로 재생 (16000 Hz, 모노, S16_LE)
        FILE* aplay = popen("aplay -f S16_LE -r 16000 -c 1 -D default", "w");
        if (aplay) {
            size_t written = fwrite(audio_buffer.data(), 1, audio_buffer.size(), aplay);
            pclose(aplay);
            if (written == audio_buffer.size()) {
                std::cout << "[Audio] Playback completed" << std::endl;
            } else {
                std::cerr << "[Audio] Playback error: wrote " << written << " / " << audio_buffer.size() << " bytes" << std::endl;
            }
        } else {
            std::cerr << "[Audio] Failed to start aplay" << std::endl;
        }
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

    // detach된 스레드들이 정리될 시간을 약간 줄 수 있음
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    RTSPRecorder recorder(logger, g_running);
    recorder.run(); // 메인 스레드 블로킹

    std::cout << "[System] 서버가 안전하게 종료되었습니다." << std::endl;
    return 0;
}