#include "rfid_monitor.h"
#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <ctime>
#include <thread>
#include <chrono>
#include "event_matcher.h"

// 생성자: 멤버 변수 초기화
RfidMonitor::RfidMonitor(std::atomic<bool>& running_flag,
                         const std::string& db_host, const std::string& db_user,
                         const std::string& db_pass, const std::string& db_name)
    : m_running(running_flag), m_socket_path("/tmp/rc522_events.sock"),
      m_db_host(db_host), m_db_user(db_user), m_db_pass(db_pass), m_db_name(db_name) {
}

RfidMonitor::~RfidMonitor() {}

void RfidMonitor::start() {
    run_loop();
}

// 현재 시간을 문자열로 반환
std::string RfidMonitor::get_current_datetime() {
    time_t now = time(nullptr);
    struct tm tstruct;
    char buf[80];
    tstruct = *localtime(&now);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tstruct);
    return std::string(buf);
}

// 간단한 JSON 파서 (라이브러리 의존성 제거)
std::string RfidMonitor::extract_json_value(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t start = json.find(search);
    if (start == std::string::npos) return "";

    start += search.length();
    if (json[start] == '"') { // 문자열 값인 경우
        start++;
        size_t end = json.find("\"", start);
        return (end == std::string::npos) ? "" : json.substr(start, end - start);
    } else { // 숫자 등인 경우
        size_t end = json.find_first_of(",}", start);
        return (end == std::string::npos) ? json.substr(start) : json.substr(start, end - start);
    }
}

// DB 저장 함수 (실제 DB 연결 로직은 여기에 구현)
void RfidMonitor::save_to_db(const std::string& uid, const std::string& age_group, const std::string& time_str) {
    // 기존 DB 저장은 유지 가능하나, 여기서는 EventMatcher에 카드 정보를 전파
    std::cout << "[DB Save Request] UID: " << uid << ", Group: " << age_group << ", Time: " << time_str << std::endl;
    // Notify matcher about RFID read (card_text is age_group or card info)
    // Pass uid as card_id
    EventMatcher::instance().on_rfid_read(uid, age_group, uid);
}

// 메인 루프 (재접속 + 데이터 수신)
void RfidMonitor::run_loop() {
    // 바깥 루프: 연결이 끊기면 다시 시도하는 역할
    while (m_running) {
        int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock_fd < 0) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        struct sockaddr_un addr = {};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, m_socket_path.c_str(), sizeof(addr.sun_path) - 1);

        // 1. 연결 시도
        if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
            perror("rc522 socket connect");
            close(sock_fd);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue; 
        }

        std::cout << ">> [RFID] 데몬 연결 성공! 데이터 수신 대기 중..." << std::endl;

        // 2. 타임아웃 설정 (Ctrl+C 즉각 반응을 위해 중요)
        // read 함수가 데이터가 없어도 1초마다 풀려나게 함
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

        char buffer[4096];
        std::string line_buffer;

        // 안쪽 루프: 연결된 상태에서 데이터 읽기
        while (m_running) {
            ssize_t n = read(sock_fd, buffer, sizeof(buffer) - 1);

            if (n > 0) {
                // [데이터 수신]
                buffer[n] = '\0';
                line_buffer += buffer;

                // 줄바꿈(\n) 단위로 잘라서 처리 (NDJSON)
                size_t pos;
                while ((pos = line_buffer.find('\n')) != std::string::npos) {
                    std::string json_line = line_buffer.substr(0, pos);
                    line_buffer.erase(0, pos + 1);
                    if (json_line.empty()) continue;

                    try {
                        std::string uid = extract_json_value(json_line, "id");
                        std::string age_group = extract_json_value(json_line, "text");
                        std::string now = get_current_datetime();

                        std::cout << ">>> [RFID Tag] UID: " << uid << " (" << age_group << ") Time: " << now << std::endl;
                        save_to_db(uid, age_group, now);
                    } catch (...) {
                        std::cerr << "[RFID] Parse Error" << std::endl;
                    }
                }
            } else if (n == -1 && !(errno == EAGAIN || errno == EWOULDBLOCK)) {
                // [에러 발생]
                perror("rc522 read");
                break; // 안쪽 루프 탈출
            } else { // n == 0
                // [연결 종료] 데몬이 꺼짐
                std::cerr << ">> [RFID] 데몬 연결 끊김. 재접속 시도..." << std::endl;
                std::cerr << "[RFID DEBUG] read() returned 0 (EOF) from socket. Closing and will retry accept." << std::endl;
                break; // 안쪽 루프 탈출
            }
        } // 안쪽 while 끝

        close(sock_fd);
        
        // 너무 빠른 재접속 방지 (CPU 보호)
        if (m_running) std::this_thread::sleep_for(std::chrono::seconds(1));
    } // 바깥 while 끝

    std::cout << ">> [RFID] 모니터링 스레드 종료." << std::endl;
}
