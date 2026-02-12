#include <iostream>
#include <filesystem>
#include <thread>
#include <atomic>
#include "log.h"
#include "recorder.h" 
#include "cleanup.h" // 분리된 cleanup 헤더 추가
#include "auth.h"
#include <arpa/inet.h>
#include <unistd.h>
#include <fstream>
#include <vector>

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
    if (!fs::exists(VOICE_SAVE_DIR)) fs::create_directories(VOICE_SAVE_DIR);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {AF_INET, htons(AUDIO_PORT), {INADDR_ANY}};
    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5);

    while (true) {
        int client_fd = accept(server_fd, NULL, NULL);
        
        // 현재 시간을 파일명으로 사용
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char time_buf[64];
        std::strftime(time_buf, sizeof(time_buf), "%Y%m%d_%H%M%S", std::localtime(&now));
        std::string filename = std::string(VOICE_SAVE_DIR) + "/voice_" + time_buf + ".raw";

        std::ofstream outfile(filename, std::ios::binary);
        char buf[4096];
        ssize_t bytes;
        while ((bytes = read(client_fd, buf, sizeof(buf))) > 0) {
            outfile.write(buf, bytes);
        }
        outfile.close();
        close(client_fd);
        std::cout << "[Audio] Received and saved: " << filename << std::endl;
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



int main() {
    // 1. 디렉토리 생성 (recorder.h에 정의된 상수 사용)
    if (!fs::exists(VIDEO_SAVE_DIR)) fs::create_directories(VIDEO_SAVE_DIR);

    // 2. DB 연결
    DBLogger logger;
    if (!logger.connect()) {
        std::cerr << "[Fatal] DB Connection failed." << std::endl;
        return -1;
    }

    // 3. 실행 플래그
    std::atomic<bool> running(true);

    // 4. 파일 정리 스레드 시작 (분리된 함수 호출)
    // VIDEO_SAVE_DIR 경로를 인자로 넘겨줍니다.
    std::thread t1(run_file_cleanup_worker, std::ref(running), std::string(VIDEO_SAVE_DIR), 300);
    t1.detach();

    // 5. DB 정리 스레드 시작
    std::thread t2([&](){ 
        while(running) { 
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
    RTSPRecorder recorder(logger, running);
    recorder.run(); // 메인 스레드 블로킹

    running = false;
    std::cout << "[System] Server shutdown." << std::endl;
    return 0;
}