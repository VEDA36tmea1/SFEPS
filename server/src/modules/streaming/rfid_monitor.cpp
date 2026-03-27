#include "rfid_monitor.h"

#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <errno.h>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include "app_services.h"
#include "analytics.h"
#include "text_utils.h"

RfidMonitor::RfidMonitor(std::atomic<bool>& running_flag, AnalyticsProcessor& analytics)
    : m_running(running_flag), m_analytics(analytics), m_socket_path("/tmp/rc522_events.sock") {}

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
    const std::string key_token = "\"" + key + "\"";
    const std::size_t key_pos = json.find(key_token);
    if (key_pos == std::string::npos) return "";

    std::size_t cursor = json.find(':', key_pos + key_token.size());
    if (cursor == std::string::npos) return "";
    ++cursor;

    while (cursor < json.size() &&
           std::isspace(static_cast<unsigned char>(json[cursor]))) {
        ++cursor;
    }
    if (cursor >= json.size()) return "";

    if (json[cursor] == '"') {
        ++cursor;
        std::string parsed;
        parsed.reserve(32);
        bool escaped = false;
        for (; cursor < json.size(); ++cursor) {
            const char c = json[cursor];
            if (escaped) {
                switch (c) {
                    case 'n':
                        parsed.push_back('\n');
                        break;
                    case 'r':
                        parsed.push_back('\r');
                        break;
                    case 't':
                        parsed.push_back('\t');
                        break;
                    case '\\':
                    case '"':
                    case '/':
                        parsed.push_back(c);
                        break;
                    default:
                        parsed.push_back(c);
                        break;
                }
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '"') {
                return parsed;
            }
            parsed.push_back(c);
        }
        return "";
    }

    const std::size_t end = json.find_first_of(",}", cursor);
    if (end == std::string::npos) {
        return trim_copy(json.substr(cursor));
    }
    return trim_copy(json.substr(cursor, end - cursor));
}

void RfidMonitor::process_rfid_tag(const std::string& uid,
                                   const std::string& card_age_text,
                                   const std::string& time_str) {
    (void)uid;
    (void)time_str;
    play_local_rfid_tag_tone();
    m_analytics.onRfidRead(card_age_text);
}

// 메인 루프 (poll 기반)
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
            // 연결 실패 로그는 운영 중 노이즈가 커서 임시 비활성화
            // perror("rc522 socket connect");
            close(sock_fd);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        std::cout << "[rfid_monitor.cpp] " << ">> [RFID] 데몬 연결 성공! 데이터 수신 대기 중..."
                  << std::endl;

        char buffer[4096];
        std::string line_buffer;

        // 안쪽 루프: 연결된 상태에서 poll로 데이터 대기
        while (m_running) {
            struct pollfd pfd = {sock_fd, POLLIN, 0};
            int poll_result = poll(&pfd, 1, 1000);  // 1000ms 타임아웃

            if (poll_result < 0) {
                // poll() 에러
                if (errno == EINTR) continue;  // 신호 재시도
                perror("rc522 poll");
                break;  // 안쪽 루프 탈출
            }
            if (poll_result == 0) {
                // 타임아웃 - 데이터 없음, 루프 계속
                continue;
            }
            if (pfd.revents & POLLIN) {
                // 읽을 데이터 있음
                ssize_t n = read(sock_fd, buffer, sizeof(buffer) - 1);

                if (n > 0) {
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
                            std::string card_age_text = extract_json_value(json_line, "text");
                            std::string device_id = extract_json_value(json_line, "device_id");
                            std::string tag_timestamp = extract_json_value(json_line, "timestamp");
                            std::string now = get_current_datetime();
                            (void)device_id;
                            (void)tag_timestamp;
                            std::cout << "[rfid_monitor.cpp] " << ">>> [RFID 태그] UID: " << uid
                                      << " (" << card_age_text << ") 시간: " << now << std::endl;

                            if (uid.empty() || card_age_text.empty()) {
                                std::cerr << "[rfid_monitor.cpp] "
                                          << "[RFID 경고] 필수 필드 누락: uid_비었음="
                                          << (uid.empty() ? "true" : "false")
                                          << ", text_비었음="
                                          << (card_age_text.empty() ? "true" : "false")
                                          << std::endl;
                            }
                            process_rfid_tag(uid, card_age_text, now);
                        } catch (...) {
                            std::cerr << "[rfid_monitor.cpp] "
                                      << "[RFID] 파싱 오류. raw="
                                      << sanitize_for_log(json_line) << std::endl;
                        }
                    }
                } else if (n == 0) {
                    // EOF: 데몬 연결 종료
                    std::cerr << ">> [RFID] 데몬 연결 끊김. 재접속 시도..." << std::endl;
                    break;  // 안쪽 루프 탈출
                } else {
                    // read() 에러
                    if (errno == EINTR) continue;
                    perror("rc522 read");
                    break;  // 안쪽 루프 탈출
                }
            } else if (pfd.revents & (POLLERR | POLLHUP)) {
                // 소켓 에러 또는 hang up
                std::cerr << ">> [RFID] 소켓 에러 (revents=" << pfd.revents << "). 재접속 시도..."
                          << std::endl;
                break;  // 안쪽 루프 탈출
            }
        }  // 안쪽 while 끝

        close(sock_fd);

        // 너무 빠른 재접속 방지 (CPU 보호)
        if (m_running) std::this_thread::sleep_for(std::chrono::seconds(1));
    }  // 바깥 while 끝

    std::cout << "[rfid_monitor.cpp] [RFID] monitor 종료." << std::endl;
}
