#include "log.h"
#include <iostream>

DBLogger::DBLogger(const char* db) : isRunning(false), conn(NULL), db_name(db) {}

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

    if (mysql_real_connect(conn, host, user, pass, db_name, 3306, NULL, 0) == NULL) {
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
        logQueue.push({SYSTEM_LOG, type, message, "", 0.0f});
    }
    cv.notify_one();
}

// 2. 로그인 로그 큐에 넣기
void DBLogger::enqueueLogin(const std::string& username, const std::string& ip, bool success) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        // value가 1.0이면 성공, 0.0이면 실패
        logQueue.push({LOGIN_LOG, username, ip, "", success ? 1.0f : 0.0f});
    }
    cv.notify_one();
}

// 3. 분석 로그 큐에 넣기
void DBLogger::enqueueAnalytics(const std::string& time, const std::string& objType, float conf, const std::string& details) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        logQueue.push({ANALYTICS_LOG, objType, details, time, conf});
    }
    cv.notify_one();
}

// [신규] 녹화 파일 기록 큐에 넣기
void DBLogger::enqueueRecording(const std::string& filename) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        // str1에 파일명 저장
        logQueue.push({RECORDING_LOG, filename, "", "", 0.0f});
    }
    cv.notify_one();
}


// [신규] DB 청소 요청을 큐에 넣기
void DBLogger::requestDbCleanup() {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        // 내용 없는 청소용 시그널 전송
        logQueue.push({CLEANUP_DB_LOG, "", "", "", 0.0f});
    }
    cv.notify_one();
}


// [핵심] XML 파싱 및 필터링 로직
void DBLogger::parseAndLogXML(const char* xmlData) {
    XMLDocument doc;
    XMLError e = doc.Parse(xmlData);
    if (e != XML_SUCCESS) return;

    // XML 구조 탐색
    XMLElement* root = doc.FirstChildElement("tt:MetadataStream");
    if (!root) return;
    XMLElement* analytics = root->FirstChildElement("tt:VideoAnalytics");
    if (!analytics) return;
    XMLElement* frame = analytics->FirstChildElement("tt:Frame");
    if (!frame) return;

    // 시간 추출 (UtcTime)
    const char* utcTime = frame->Attribute("UtcTime");
    std::string frameTime = (utcTime) ? utcTime : "";
    
    // 시간 포맷 정리 (T -> 공백, 초 뒤에 잘라내기)
    size_t t_pos = frameTime.find('T');
    if (t_pos != std::string::npos) frameTime[t_pos] = ' ';
    if (frameTime.length() > 19) frameTime = frameTime.substr(0, 19);

    // 객체 루프
    XMLElement* obj = frame->FirstChildElement("tt:Object");
    while (obj) {
        XMLElement* appearance = obj->FirstChildElement("tt:Appearance");
        if (appearance) {
            XMLElement* classElem = appearance->FirstChildElement("tt:Class");
            if (classElem) {
                XMLElement* typeElem = classElem->FirstChildElement("tt:Type");
                if (typeElem) {
                    const char* typeName = typeElem->GetText();
                    float likelihood = typeElem->FloatAttribute("Likelihood");

                    // ★ 필터링 조건: Human이고 확률 0.8 이상만 저장
                    if (typeName && std::string(typeName) == "Human" && likelihood >= 0.8) {
                        
                        std::string details = "";
                        XMLElement* body = appearance->FirstChildElement("tt:HumanBody");
                        if (body) {
                            XMLElement* gender = body->FirstChildElement("bd:Gender");
                            if (gender && gender->GetText()) details += "Gender:" + std::string(gender->GetText()) + " ";
                            
                            XMLElement* clothing = body->FirstChildElement("bd:Clothing");
                            if (clothing) {
                                XMLElement* tops = clothing->FirstChildElement("bd:Tops");
                                if (tops) {
                                    XMLElement* color = tops->FirstChildElement("tt:ColorString");
                                    if(color && color->GetText()) details += "Top:" + std::string(color->GetText()) + " ";
                                }
                                XMLElement* bottoms = clothing->FirstChildElement("bd:Bottoms");
                                if (bottoms) {
                                    XMLElement* color = bottoms->FirstChildElement("tt:ColorString");
                                    if(color && color->GetText()) details += "Bot:" + std::string(color->GetText());
                                }
                            }
                        }
                        // 큐에 전송
                        enqueueAnalytics(frameTime, "Human", likelihood, details);
                    }
                }
            }
        }
        obj = obj->NextSiblingElement("tt:Object");
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

        if (conn) {
            std::string query;
            
            if (item.type == SYSTEM_LOG) {
                // 일반 로그
                query = "INSERT INTO logs (event_type, message) VALUES ('" + item.str1 + "', '" + item.str2 + "')";
            } 
            else if (item.type == LOGIN_LOG) {
                // 로그인 로그
                std::string status = (item.value > 0.5f) ? "SUCCESS" : "FAIL";
                query = "INSERT INTO login_logs (username, ip_address, status) VALUES ('" + item.str1 + "', '" + item.str2 + "', '" + status + "')";
            }
            else if (item.type == ANALYTICS_LOG) {
                // ★ 분석 로그 (frame_time 컬럼 사용 확인!)
                query = "INSERT INTO analytics_logs (frame_time, object_type, confidence, details) VALUES ('" 
                        + item.time_str + "', '" + item.str1 + "', " + std::to_string(item.value) + ", '" + item.str2 + "')";
            }

             else if (item.type == RECORDING_LOG) {
                // [신규] 녹화 테이블에 저장
                query = "INSERT INTO recordings (filename) VALUES ('" + item.str1 + "')";
            }
            else if (item.type == CLEANUP_DB_LOG) {
                // [핵심] 100MB 넘으면 삭제하는 로직
                // 1. analytics_logs 테이블 용량 계산 (MB 단위)
                std::string sizeQuery = "SELECT (data_length + index_length) / 1024 / 1024 FROM information_schema.tables WHERE table_schema = '" + std::string(db_name) + "' AND table_name = 'analytics_logs'";
                
                if (mysql_query(conn, sizeQuery.c_str()) == 0) {
                    MYSQL_RES* res = mysql_store_result(conn);
                    if (res) {
                        MYSQL_ROW row = mysql_fetch_row(res);
                        if (row && row[0]) {
                            double sizeMB = std::stod(row[0]);
                            
                            // 2. 100MB 초과 시 삭제
                            if (sizeMB > 100.0) {
                                std::cout << "[DB Cleanup] Table size " << sizeMB << "MB > 100MB. Deleting old rows..." << std::endl;
                                // 가장 오래된 2000개 삭제 (한 번에 많이 지우면 락 걸림 방지)
                                std::string delQuery = "DELETE FROM analytics_logs ORDER BY id ASC LIMIT 2000";
                                mysql_query(conn, delQuery.c_str());
                            }
                        }
                        mysql_free_result(res);
                    }
                }
                continue; // 삭제 쿼리는 위에서 실행했으므로 건너뜀
            }

            if (mysql_query(conn, query.c_str())) {
                std::cerr << "[DB Error] " << mysql_error(conn) << std::endl;
            }
        }
    }
}