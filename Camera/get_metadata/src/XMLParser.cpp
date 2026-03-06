#include "XMLParser.h"
#include "Config.h"
#include <iostream>
#include <time.h>

std::string XMLParser::get_current_time_str() {
    time_t now = time(0);
    struct tm tstruct;
    char buf[80];
    tstruct = *localtime(&now);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tstruct);
    return std::string(buf);
}

std::vector<DetectedObject> XMLParser::parseAndProcess(std::string& accumulated_xml, unsigned int last_timestamp) {
    
    std::vector<DetectedObject> results;

    try {
        size_t search_pos = 0;
        while (true) {
            size_t obj_start = accumulated_xml.find("<tt:Object", search_pos);
            if (obj_start == std::string::npos) break;

            // ID 추출
            std::string obj_id = "Unknown";
            size_t id_pos = accumulated_xml.find("ObjectId=\"", obj_start);
            if (id_pos != std::string::npos) {
                size_t start = id_pos + 10;
                size_t end = accumulated_xml.find("\"", start);
                obj_id = accumulated_xml.substr(start, end - start);
            }

            // 타입 추출
            std::string obj_type = "Unknown";
            size_t type_pos = accumulated_xml.find("<tt:Type>", obj_start);
            size_t next_obj = accumulated_xml.find("<tt:Object", obj_start + 1);
            if (type_pos != std::string::npos && (next_obj == std::string::npos || type_pos < next_obj)) {
                size_t start = type_pos + 9;
                size_t end = accumulated_xml.find("</tt:Type>", start);
                obj_type = accumulated_xml.substr(start, end - start);
            }

            float x = -1, y = -1, w = -1, h = -1;
            size_t x_pos = accumulated_xml.find("x=\"", obj_start);
            size_t y_pos = accumulated_xml.find("y=\"", obj_start);
            
            if (x_pos != std::string::npos && y_pos != std::string::npos && (next_obj == std::string::npos || x_pos < next_obj)) {
                size_t end_x = accumulated_xml.find("\"", x_pos + 3);
                float raw_x = std::stof(accumulated_xml.substr(x_pos + 3, end_x - (x_pos + 3)));
                size_t end_y = accumulated_xml.find("\"", y_pos + 3);
                float raw_y = std::stof(accumulated_xml.substr(y_pos + 3, end_y - (y_pos + 3)));

                // 🌟 정규화 제거: 픽셀 원본 값 그대로 대입
                x = raw_x; 
                y = raw_y;
            }

            // 🌟 2. 바운딩 박스 크기(width, height) 추출 (ONVIF 표준 left, right, top, bottom 계산)
            size_t left_pos   = accumulated_xml.find("left=\"", obj_start);
            size_t right_pos  = accumulated_xml.find("right=\"", obj_start);
            size_t top_pos    = accumulated_xml.find("top=\"", obj_start);
            size_t bottom_pos = accumulated_xml.find("bottom=\"", obj_start);

            if (left_pos != std::string::npos && right_pos != std::string::npos && 
                top_pos != std::string::npos && bottom_pos != std::string::npos && 
                (next_obj == std::string::npos || left_pos < next_obj)) 
            {
                size_t end_left   = accumulated_xml.find("\"", left_pos + 6);
                size_t end_right  = accumulated_xml.find("\"", right_pos + 7);
                size_t end_top    = accumulated_xml.find("\"", top_pos + 5);
                size_t end_bottom = accumulated_xml.find("\"", bottom_pos + 8);

                float left   = std::stof(accumulated_xml.substr(left_pos + 6, end_left - (left_pos + 6)));
                float right  = std::stof(accumulated_xml.substr(right_pos + 7, end_right - (right_pos + 7)));
                float top    = std::stof(accumulated_xml.substr(top_pos + 5, end_top - (top_pos + 5)));
                float bottom = std::stof(accumulated_xml.substr(bottom_pos + 8, end_bottom - (bottom_pos + 8)));

                // 🌟 가로(폭) = 오른쪽 끝 - 왼쪽 끝 / 세로(높이) = 맨 밑 - 맨 위
                w = right - left;
                h = bottom - top;
                
            }

            // 🌟 출력 스위치 ON (true로 고정) 및 크기(Size) 로그 추가
            constexpr bool k_enable_object_log = true;
            if (k_enable_object_log && x != -1 && y != -1 && obj_type == "Human") {
                bool is_new_id = (log_timer_map.find(obj_id) == log_timer_map.end());
                if (is_new_id || (last_timestamp - log_timer_map[obj_id] > LOG_THROTTLE)) {
                    std::string prefix = is_new_id ? "✨ [NEW]" : "🎯 [OBJ]";
                    std::cout << prefix << " ID: " << obj_id 
                              << " | Type: " << obj_type 
                              << " | Pos: (" << x << ", " << y << ")" 
                              << " | Bbox_center: (" << w << "x" << h << ")" // 🌟 크기 출력 추가
                              << " | RTP: " << last_timestamp 
                              << " | Time: " << get_current_time_str() << std::endl;
                    log_timer_map[obj_id] = last_timestamp;
                }
            }
            search_pos = obj_start + 1;
        }
    

        // =========================================================
        // [PART 2] 이벤트(Event) 파싱 (활성화됨)
        // =========================================================
        // size_t search_pos = 0;
        while (true) {
            size_t msg_start = accumulated_xml.find("<wsnt:NotificationMessage", search_pos);
            if (msg_start == std::string::npos) break;

            size_t msg_end = accumulated_xml.find("</wsnt:NotificationMessage>", msg_start);
            if (msg_end == std::string::npos) break;

            std::string message_block = accumulated_xml.substr(msg_start, msg_end - msg_start);

            // RuleName 추출
            std::string rule_name = "Unknown";
            size_t name_item_pos = message_block.find("Name=\"RuleName\"");
            if (name_item_pos != std::string::npos) {
                size_t val_pos = message_block.find("Value=\"", name_item_pos);
                if (val_pos != std::string::npos) {
                    size_t start = val_pos + 7;
                    size_t end = message_block.find("\"", start);
                    rule_name = message_block.substr(start, end - start);
                }
            }

            // State 추출
            bool is_active = false;
            size_t state_item_pos = message_block.find("Name=\"State\"");
            if (state_item_pos != std::string::npos) {
                size_t val_pos = message_block.find("Value=\"", state_item_pos);
                if (val_pos != std::string::npos) {
                    size_t start = val_pos + 7;
                    size_t end = message_block.find("\"", start);
                    std::string state_val = message_block.substr(start, end - start);
                    if (state_val == "true" || state_val == "1") is_active = true;
                }
            }

            // ObjectId 추출
            std::string triggered_id = "None";
            size_t id_item_pos = message_block.find("Name=\"ObjectId\"");
            if (id_item_pos != std::string::npos) {
                size_t val_pos = message_block.find("Value=\"", id_item_pos);
                if (val_pos != std::string::npos) {
                    size_t start = val_pos + 7;
                    size_t end = message_block.find("\"", start);
                    triggered_id = message_block.substr(start, end - start);
                }
            }

            // 결과 출력
            if (rule_name != "Unknown" && is_active) {
                unsigned int time_diff = last_timestamp - gate_last_pass_time[rule_name];
                
                if (time_diff < TAILGATE_LIMIT && gate_last_pass_time[rule_name] != 0) {
                    float diff_sec = (float)time_diff / 90000.0f;
                    
                    std::cout << "🚨 [TAILGATING] " << rule_name 
                              << " | Trigger ID: " << triggered_id
                              << " | RTP: " << last_timestamp 
                              << " | Gap: " << diff_sec << "s" << std::endl;
                } else {
                    std::cout << "✅ [EVENT] " << rule_name 
                              << " Active | ID: " << triggered_id 
                              << " | RTP: " << last_timestamp 
                              << " | Time: " << get_current_time_str() << std::endl;
                }
                gate_last_pass_time[rule_name] = last_timestamp;
            }

            search_pos = msg_end;
        }
    } catch (...) {}
    
    return results;
}