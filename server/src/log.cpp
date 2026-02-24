#include "log.h"
#include <iostream>
#include <cstdlib>

using namespace tinyxml2;
#include <sstream>
#include <vector>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <random>
#include "event_matcher.h"

DBLogger::DBLogger(const char* db) : isRunning(false), conn(NULL), db_name(db) {
    // Camera resolution is defined in code (include/log.h)
    // Change cam_width / cam_height in include/log.h if you need a different value.
    std::cout << "[DBLogger] Camera resolution (from code) set to " << cam_width << "x" << cam_height << std::endl;
}

DBLogger::~DBLogger() {
    isRunning = false;
    cv.notify_one(); 
    if (workerThread.joinable()) workerThread.join();
    if (conn != NULL) {
        mysql_close(conn);
        std::cout << "[System] DB Connection Closed." << std::endl;
    }
}

bool DBLogger::connect() {
    conn = mysql_init(NULL);
    if (conn == NULL) return false;

    if (mysql_real_connect(conn, host, user, pass, db_name, 0, NULL, 0) == NULL) {
        std::cerr << "[DB Error] " << mysql_error(conn) << std::endl;
        return false;
    }
    
    std::cout << "[System] DB Connected. Worker Thread Started." << std::endl;
    isRunning = true;
    workerThread = std::thread(&DBLogger::processQueue, this);
    return true;
}

// 1. 일반 로그 큐에 넣기
void DBLogger::enqueue(const std::string& type, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        // 구조체 순서: type, str1, str2, time_str, x, y, event, age, photo_path, login_success
        logQueue.push({SYSTEM_LOG, type, message, "", 0, 0, "", 0, "", false});
    }
    cv.notify_one();
}

// 2. 로그인 로그 큐에 넣기
void DBLogger::enqueueLogin(const std::string& username, const std::string& ip, bool success) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        // login_success 필드(맨 마지막)에 success 값 전달
        logQueue.push({LOGIN_LOG, username, ip, "", 0, 0, "", 0, "", success});
    }
    cv.notify_one();
}

// 3. ★ [수정됨] 분석 로그 큐에 넣기
// 인자가 x, y, event, age, photoPath로 변경됨
void DBLogger::enqueueAnalytics(const std::string& time, const std::string& objType, 
                                float x, float y, const std::string& event, 
                                int age, const std::string& photoPath) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        // str2(기존 details)는 비워둡니다.
        logQueue.push({ANALYTICS_LOG, objType, "", time, x, y, event, age, photoPath, false});
    }
    cv.notify_one();
}

// [신규] 녹화 파일 기록 큐에 넣기
void DBLogger::enqueueRecording(const std::string& filename) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        logQueue.push({RECORDING_LOG, filename, "", "", 0, 0, "", 0, "", false});
    }
    cv.notify_one();
}

// [신규] DB 청소 요청을 큐에 넣기
void DBLogger::requestDbCleanup() {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        logQueue.push({CLEANUP_DB_LOG, "", "", "", 0, 0, "", 0, "", false});
    }
    cv.notify_one();
}


// [핵심] XML 파싱 및 필터링 로직
// ★ 주의: XML에 x,y 좌표나 나이 정보가 없다면 기본값(0)을 넣어야 합니다.
void DBLogger::parseAndLogXML(const char* xmlData) {
    if (!xmlData) return;

    // 카메라 해상도: 클래스 멤버값 사용
    const int CAM_W = cam_width;
    const int CAM_H = cam_height;

    auto trim = [](const std::string &s) {
        size_t a = 0; while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
        size_t b = s.size(); while (b > a && std::isspace((unsigned char)s[b-1])) --b;
        return s.substr(a, b - a);
    };

    std::istringstream iss(xmlData);
    std::string line;
    while (std::getline(iss, line)) {
        std::string l = trim(line);
        if (l.empty()) continue;

        // 태그 추출 예: [NEW], [EVENT], [OBJ]
        std::string tag;
        if (!l.empty() && l.front() == '[') {
            size_t p = l.find(']');
            if (p != std::string::npos) {
                tag = l.substr(1, p - 1);
                l = trim(l.substr(p + 1));
                // 가능하면 구분자 제거
                if (!l.empty() && l.front() == '|') l = trim(l.substr(1));
            }
        }

        // 파트 분리: " | " 기준
        std::vector<std::string> parts;
        size_t start = 0;
        while (start < l.size()) {
            size_t sep = l.find(" | ", start);
            if (sep == std::string::npos) {
                parts.push_back(trim(l.substr(start)));
                break;
            }
            parts.push_back(trim(l.substr(start, sep - start)));
            start = sep + 3;
        }

        std::string id, type, time_str, event_str, photo_path = "";
        int x = 0, y = 0; // 픽셀 좌표 (정수화)
        int estimated_age = 0;

        for (const auto &part : parts) {
            if (part.empty()) continue;

            size_t colon = part.find(':');
            if (colon == std::string::npos) {
                // 콜론이 없으면 이벤트 설명일 가능성
                if (event_str.empty()) event_str = part;
                continue;
            }

            std::string key = trim(part.substr(0, colon));
            std::string val = trim(part.substr(colon + 1));

            if (key == "ID") {
                id = val;
            } else if (key == "Type") {
                type = val;
            } else if (key == "Pos") {
                // 형식: (0.0195312, 0.891204)
                size_t a = val.find('(');
                size_t b = val.find(')');
                std::string coords = val;
                if (a != std::string::npos && b != std::string::npos && b > a)
                    coords = val.substr(a + 1, b - a - 1);
                size_t comma = coords.find(',');
                if (comma != std::string::npos) {
                    std::string xs = trim(coords.substr(0, comma));
                    std::string ys = trim(coords.substr(comma + 1));
                    try {
                        double nx = std::stod(xs);
                        double ny = std::stod(ys);
                        x = static_cast<int>(nx * CAM_W);
                        y = static_cast<int>(ny * CAM_H);
                    } catch (...) {}
                }
            } else if (key == "Time") {
                time_str = val;
            } else if (key == "RTP") {
                // 무시
            } else if (key == "Event") {
                event_str = val;
            } else if (key == "Age") {
                try { estimated_age = std::stoi(val); } catch(...) {}
            } else {
                // 기타 키: 무시하거나 이벤트로 저장
                if (event_str.empty()) event_str = val;
            }
        }

        // 태그 기반 보완
        if (!tag.empty()) {
            if (tag == "NEW") {
                if (event_str.empty()) event_str = "NEW";
            } else if (tag == "OBJ") {
                if (event_str.empty()) event_str = "OBJ";
            } else if (tag == "EVENT") {
                if (event_str.empty()) event_str = "EVENT";
            }
        }

        if (type.empty()) type = "Unknown";
        if (event_str.empty()) event_str = "Detected";

        // 이벤트 명에서 'first' / 'second' 추출
        std::string lower_event = event_str;
        std::transform(lower_event.begin(), lower_event.end(), lower_event.begin(), ::tolower);
        size_t p_first = lower_event.find("first");
        size_t p_second = lower_event.find("second");

        if (p_first != std::string::npos) {
            std::string gate = event_str.substr(0, p_first);
            gate.erase(std::remove_if(gate.begin(), gate.end(), ::isspace), gate.end());
            // 랜덤 연령 그룹 할당
            static std::mt19937 rng((std::random_device())());
            static std::vector<std::string> ages = {"Adult", "Senior", "Youth"};
            std::uniform_int_distribution<int> dist(0, (int)ages.size()-1);
            std::string assigned = ages[dist(rng)];
            EventMatcher::instance().register_first(gate, id.empty() ? "" : id, assigned);
        }

        if (p_second != std::string::npos) {
            std::string gate = event_str.substr(0, p_second);
            gate.erase(std::remove_if(gate.begin(), gate.end(), ::isspace), gate.end());
            std::string msg;
            EventMatcher::instance().on_second(gate, msg);
            // msg already sent inside matcher if mismatch
        }

        // DB에 저장 (enqueueAnalytics expects: time, objType, x, y, event, age, photoPath)
        enqueueAnalytics(time_str, type, static_cast<float>(x), static_cast<float>(y), event_str, estimated_age, photo_path);
    }
}

// 일꾼 스레드 (실제 DB 저장)
void DBLogger::processQueue() {
    while (isRunning) {
        LogItem item;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            cv.wait(lock, [this] { return !logQueue.empty() || !isRunning; });
            if (!isRunning && logQueue.empty()) break;
            item = logQueue.front();
            logQueue.pop();
        }

        if (!conn) continue;

        std::string query;

        if (item.type == SYSTEM_LOG) {
            std::string esc1 = item.str1;
            std::string esc2 = item.str2;
            query = "INSERT INTO logs (event_type, message) VALUES ('" + esc1 + "', '" + esc2 + "')";
        } else if (item.type == LOGIN_LOG) {
            std::string status = (item.login_success) ? "SUCCESS" : "FAIL";
            query = "INSERT INTO login_logs (username, ip_address, status) VALUES ('" + item.str1 + "', '" + item.str2 + "', '" + status + "')";
        } else if (item.type == ANALYTICS_LOG) {
            // Escape string fields using mysql_real_escape_string
            auto escape = [&](const std::string &s) {
                std::string out;
                out.resize(s.size() * 2 + 1);
                unsigned long new_len = mysql_real_escape_string(conn, &out[0], s.c_str(), s.size());
                out.resize(new_len);
                return out;
            };

            std::string esc_obj = escape(item.str1);
            std::string esc_photo = escape(item.photo_path);
            std::string esc_event = escape(item.event);

            std::ostringstream oss;
            oss << "INSERT INTO analytics_logs (frame_time, object_type, created_at, estimated_age, photo_path, x, y, event) VALUES ('";
            oss << escape(item.time_str) << "', '" << esc_obj << "', NOW(), '" << item.age << "', '" << esc_photo << "', ";
            oss << std::fixed << std::setprecision(2) << item.x << ", " << item.y << ", '" << esc_event << "')";
            query = oss.str();
        } else if (item.type == RECORDING_LOG) {
            std::string esc_fn = item.str1;
            query = "INSERT INTO recordings (filename) VALUES ('" + esc_fn + "')";
        } else if (item.type == CLEANUP_DB_LOG) {
            std::string sizeQuery = "SELECT (data_length + index_length) / 1024 / 1024 FROM information_schema.tables WHERE table_schema = '" + std::string(db_name) + "' AND table_name = 'analytics_logs'";
            if (mysql_query(conn, sizeQuery.c_str()) == 0) {
                MYSQL_RES* res = mysql_store_result(conn);
                if (res) {
                    MYSQL_ROW row = mysql_fetch_row(res);
                    if (row) {
                        // 기존 동작 유지 — 추가 로직이 필요하면 여기서 처리
                    }
                    mysql_free_result(res);
                }
            }
            continue;
        }

        if (!query.empty()) {
            if (mysql_query(conn, query.c_str())) {
                std::cerr << "[DB Error] " << mysql_error(conn) << std::endl;
                std::cout << "Query: " << query << std::endl;
            }
        }
    }
}