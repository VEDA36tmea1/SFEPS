#include "XMLParser.h"
#include "Config.h"
#include <iostream>
#include <time.h>

namespace {
std::size_t find_in_range(const std::string& raw,
                          const char* needle,
                          std::size_t start_pos,
                          std::size_t end_pos) {
    const std::size_t pos = raw.find(needle, start_pos);
    if (pos == std::string::npos || pos >= end_pos) return std::string::npos;
    return pos;
}

bool parse_float_attr(const std::string& raw,
                      std::size_t attr_pos,
                      std::size_t value_offset,
                      float& out_value) {
    if (attr_pos == std::string::npos) return false;

    const std::size_t value_start = attr_pos + value_offset;
    const std::size_t value_end = raw.find("\"", value_start);
    if (value_end == std::string::npos) return false;

    try {
        out_value = std::stof(raw.substr(value_start, value_end - value_start));
    } catch (...) {
        return false;
    }
    return true;
}
}

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
        // 🌟 태그 시간(UtcTime) 파싱
        std::string tag_time = "Unknown";
        size_t utc_pos = accumulated_xml.find("UtcTime=\"");
        if (utc_pos != std::string::npos) {
            size_t start = utc_pos + 9;
            size_t end = accumulated_xml.find("\"", start);
            if (end != std::string::npos) {
                tag_time = accumulated_xml.substr(start, end - start);
            }
        }

        size_t search_pos = 0;
        while (true) {
            size_t obj_start = accumulated_xml.find("<tt:Object", search_pos);
            if (obj_start == std::string::npos) break;

            std::string obj_id = "Unknown";
            size_t id_pos = accumulated_xml.find("ObjectId=\"", obj_start);
            if (id_pos != std::string::npos) {
                size_t start = id_pos + 10;
                size_t end = accumulated_xml.find("\"", start);
                obj_id = accumulated_xml.substr(start, end - start);
            }

            std::string obj_type = "Unknown";
            size_t type_pos = accumulated_xml.find("<tt:Type>", obj_start);
            size_t next_obj = accumulated_xml.find("<tt:Object", obj_start + 1);
            if (type_pos != std::string::npos && (next_obj == std::string::npos || type_pos < next_obj)) {
                size_t start = type_pos + 9;
                size_t end = accumulated_xml.find("</tt:Type>", start);
                obj_type = accumulated_xml.substr(start, end - start);
            }

            float x = -1, y = -1, w = -1, h = -1;
            float left = -1, right = -1, top = -1, bottom = -1;
            size_t x_pos = accumulated_xml.find("x=\"", obj_start);
            size_t y_pos = accumulated_xml.find("y=\"", obj_start);
            
            if (x_pos != std::string::npos && y_pos != std::string::npos && (next_obj == std::string::npos || x_pos < next_obj)) {
                size_t end_x = accumulated_xml.find("\"", x_pos + 3);
                x = std::stof(accumulated_xml.substr(x_pos + 3, end_x - (x_pos + 3)));
                size_t end_y = accumulated_xml.find("\"", y_pos + 3);
                y = std::stof(accumulated_xml.substr(y_pos + 3, end_y - (y_pos + 3)));
            }

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
                
                left   = std::stof(accumulated_xml.substr(left_pos + 6, end_left - (left_pos + 6)));
                right  = std::stof(accumulated_xml.substr(right_pos + 7, end_right - (right_pos + 7)));
                top    = std::stof(accumulated_xml.substr(top_pos + 5, end_top - (top_pos + 5)));
                bottom = std::stof(accumulated_xml.substr(bottom_pos + 8, end_bottom - (bottom_pos + 8)));
                
                w = right - left;
                h = bottom - top;
            }

            // =========================================================
            // 🌟 Vector(속도/관성) 기반 객체 추적 및 ID 복구 알고리즘
            // =========================================================
            std::string real_id = obj_id; 

            if (x != -1 && y != -1 && obj_type == "Human") {
                if (tracking_map.find(obj_id) == tracking_map.end()) {
                    std::string matched_old_id = "";
                    float min_error = 99999.0f;

                    for (auto& pair : tracking_map) {
                        const std::string& old_id = pair.first;
                        auto& track = pair.second;

                        unsigned int dt = last_timestamp - track.last_rtp;

                        if (dt > 0 && dt < 225000) {
                            float expected_x = track.last_x + (track.vx * dt);
                            float expected_y = track.last_y + (track.vy * dt);

                            float err_x = std::abs(expected_x - x);
                            float err_y = std::abs(expected_y - y);
                            float error_dist = std::sqrt((err_x * err_x) + (err_y * err_y));

                            if (error_dist < 200.0f && error_dist < min_error) {
                                min_error = error_dist;
                                matched_old_id = old_id;
                            }
                        }
                    }

                    if (matched_old_id != "") {
                        tracking_map[obj_id] = tracking_map[matched_old_id]; 
                        tracking_map.erase(matched_old_id); 
                        real_id = tracking_map[obj_id].original_id; 
                        std::cout << "🔗 [ID 복구 성공] 카메라 ID: " << obj_id << " -> 오리지널 ID: " << real_id << " 매칭됨!" << std::endl;
                    } else {
                        tracking_map[obj_id] = {obj_id, x, y, 0.0f, 0.0f, last_timestamp};
                    }
                } else {
                    auto& track = tracking_map[obj_id];
                    unsigned int dt = last_timestamp - track.last_rtp;

                    if (dt > 0) {
                        float current_vx = (x - track.last_x) / (float)dt;
                        float current_vy = (y - track.last_y) / (float)dt;
                        track.vx = (track.vx * 0.7f) + (current_vx * 0.3f);
                        track.vy = (track.vy * 0.7f) + (current_vy * 0.3f);
                    }
                    track.last_x = x;
                    track.last_y = y;
                    track.last_rtp = last_timestamp;
                    real_id = track.original_id; 
                }

                results.push_back({real_id, obj_type, x, y});

                // 🌟 출력 스위치 무조건 ON
                constexpr bool k_enable_object_log = true;
                bool is_new_id = (log_timer_map.find(real_id) == log_timer_map.end());
                if (k_enable_object_log && (is_new_id || (last_timestamp - log_timer_map[real_id] > LOG_THROTTLE))) {
                    std::string prefix = is_new_id ? "✨ [NEW]" : "🎯 [OBJ]";
                    std::cout << prefix << " ID: " << real_id 
                              << " | Type: " << obj_type
                              << " | Pos: (" << x << ", " << y << ")" 
                              << " | BBox: (left=" << left
                              << ", right=" << right
                              << ", top=" << top
                              << ", bottom=" << bottom << ")"
                              << " | Size: (" << w << "x" << h << ")" 
                              << " | TagTime: " << tag_time << std::endl;
                    log_timer_map[real_id] = last_timestamp;
                }
            }
            search_pos = obj_start + 1;
        }

        // 🌟 GC (가비지 컬렉터) 
        for (auto it = tracking_map.begin(); it != tracking_map.end(); ) {
            if (last_timestamp - it->second.last_rtp > 450000) {
                it = tracking_map.erase(it);
            } else {
                ++it;
            }
        }

        // =========================================================
        // [PART 2] 이벤트(Event) 파싱 
        // =========================================================
        search_pos = 0;
        while (true) {
            size_t msg_start = accumulated_xml.find("<wsnt:NotificationMessage", search_pos);
            if (msg_start == std::string::npos) break;

            size_t msg_end = accumulated_xml.find("</wsnt:NotificationMessage>", msg_start);
            if (msg_end == std::string::npos) break;

            std::string message_block = accumulated_xml.substr(msg_start, msg_end - msg_start);

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
                              << " | TagTime: " << tag_time << std::endl;
                }
                gate_last_pass_time[rule_name] = last_timestamp;
            }

            search_pos = msg_end;
        }
    } catch (...) {}
    
    return results; 
}

std::vector<ParsedMetadataObject> XMLParser::parseHumanObjectsForAnalytics(const std::string& xml) const {
    std::vector<ParsedMetadataObject> results;
    results.reserve(8);
    std::size_t search_pos = 0;

    while (true) {
        const std::size_t obj_start = xml.find("<tt:Object", search_pos);
        if (obj_start == std::string::npos) break;

        const std::size_t next_obj = xml.find("<tt:Object", obj_start + 1);
        const std::size_t obj_end = (next_obj == std::string::npos) ? xml.size() : next_obj;
        ParsedMetadataObject object = {"", "", -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};

        const std::size_t id_pos = find_in_range(xml, "ObjectId=\"", obj_start, obj_end);
        if (id_pos != std::string::npos) {
            const std::size_t start = id_pos + 10;
            const std::size_t end = xml.find("\"", start);
            if (end != std::string::npos && end < obj_end) {
                object.id = xml.substr(start, end - start);
            }
        }

        const std::size_t type_pos = find_in_range(xml, "<tt:Type>", obj_start, obj_end);
        if (type_pos != std::string::npos) {
            const std::size_t start = type_pos + 9;
            const std::size_t end = xml.find("</tt:Type>", start);
            if (end != std::string::npos && end < obj_end) {
                object.type = xml.substr(start, end - start);
            }
        }

        const std::size_t x_pos = find_in_range(xml, "x=\"", obj_start, obj_end);
        const std::size_t y_pos = find_in_range(xml, "y=\"", obj_start, obj_end);
        const std::size_t left_pos = find_in_range(xml, "left=\"", obj_start, obj_end);
        const std::size_t right_pos = find_in_range(xml, "right=\"", obj_start, obj_end);
        const std::size_t top_pos = find_in_range(xml, "top=\"", obj_start, obj_end);
        const std::size_t bottom_pos = find_in_range(xml, "bottom=\"", obj_start, obj_end);

        const bool has_x = parse_float_attr(xml, x_pos, 3, object.x);
        const bool has_y = parse_float_attr(xml, y_pos, 3, object.y);
        const bool has_left = parse_float_attr(xml, left_pos, 6, object.left);
        const bool has_right = parse_float_attr(xml, right_pos, 7, object.right);
        const bool has_top = parse_float_attr(xml, top_pos, 5, object.top);
        const bool has_bottom = parse_float_attr(xml, bottom_pos, 8, object.bottom);

        if (!object.id.empty() && object.type == "Human" && has_x && has_y &&
            has_left && has_right && has_top && has_bottom) {
            results.push_back(object);
        }

        search_pos = obj_end;
    }

    return results;
}
