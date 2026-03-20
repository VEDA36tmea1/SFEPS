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
        } catch (...) { return false; }
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

// parseAndProcess: app(main.cpp)용 — IoU 트래킹 유지
std::vector<DetectedObject> XMLParser::parseAndProcess(std::string& accumulated_xml, unsigned int last_timestamp) {
    std::vector<DetectedObject> results;
    try {
        std::string tag_time = "Unknown";
        size_t utc_pos = accumulated_xml.find("UtcTime=\"");
        if (utc_pos != std::string::npos) {
            size_t start = utc_pos + 9;
            size_t end = accumulated_xml.find("\"", start);
            if (end != std::string::npos)
                tag_time = accumulated_xml.substr(start, end - start);
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
            size_t next_obj = accumulated_xml.find("<tt:Object", obj_start + 1);
            size_t type_pos = accumulated_xml.find("<tt:Type Likelihood=", obj_start);
            if (type_pos != std::string::npos && (next_obj == std::string::npos || type_pos < next_obj)) {
                size_t start = accumulated_xml.find(">", type_pos) + 1;
                size_t end = accumulated_xml.find("</tt:Type>", start);
                if (end != std::string::npos) obj_type = accumulated_xml.substr(start, end - start);
            }

            float likelihood = 1.0f;
            size_t likelihood_pos = accumulated_xml.find("<tt:Likelihood>", obj_start);
            if (likelihood_pos != std::string::npos && (next_obj == std::string::npos || likelihood_pos < next_obj)) {
                size_t start = likelihood_pos + 15;
                size_t end = accumulated_xml.find("</tt:Likelihood>", start);
                if (end != std::string::npos) {
                    try { likelihood = std::stof(accumulated_xml.substr(start, end - start)); }
                    catch (...) { likelihood = 1.0f; }
                }
            }

            float x = -1, y = -1, w = -1, h = -1;
            float left = -1, right = -1, top = -1, bottom = -1;

            size_t cog_pos = accumulated_xml.find("CenterOfGravity", obj_start);
            if (cog_pos != std::string::npos && (next_obj == std::string::npos || cog_pos < next_obj)) {
                size_t x_pos = accumulated_xml.find("x=\"", cog_pos);
                size_t y_pos = accumulated_xml.find("y=\"", cog_pos);
                if (x_pos != std::string::npos && (next_obj == std::string::npos || x_pos < next_obj)) {
                    size_t end_x = accumulated_xml.find("\"", x_pos + 3);
                    x = std::stof(accumulated_xml.substr(x_pos + 3, end_x - (x_pos + 3)));
                }
                if (y_pos != std::string::npos && (next_obj == std::string::npos || y_pos < next_obj)) {
                    size_t end_y = accumulated_xml.find("\"", y_pos + 3);
                    y = std::stof(accumulated_xml.substr(y_pos + 3, end_y - (y_pos + 3)));
                }
            }

            size_t left_pos   = accumulated_xml.find("left=\"",   obj_start);
            size_t right_pos  = accumulated_xml.find("right=\"",  obj_start);
            size_t top_pos    = accumulated_xml.find("top=\"",    obj_start);
            size_t bottom_pos = accumulated_xml.find("bottom=\"", obj_start);
            if (left_pos != std::string::npos && right_pos != std::string::npos &&
                top_pos  != std::string::npos && bottom_pos != std::string::npos &&
                (next_obj == std::string::npos || left_pos < next_obj)) {
                size_t el = accumulated_xml.find("\"", left_pos   + 6);
                size_t er = accumulated_xml.find("\"", right_pos  + 7);
                size_t et = accumulated_xml.find("\"", top_pos    + 5);
                size_t eb = accumulated_xml.find("\"", bottom_pos + 8);
                left   = std::stof(accumulated_xml.substr(left_pos   + 6, el - (left_pos   + 6)));
                right  = std::stof(accumulated_xml.substr(right_pos  + 7, er - (right_pos  + 7)));
                top    = std::stof(accumulated_xml.substr(top_pos    + 5, et - (top_pos    + 5)));
                bottom = std::stof(accumulated_xml.substr(bottom_pos + 8, eb - (bottom_pos + 8)));
                w = right - left;
                h = bottom - top;
            }

            if (x != -1 && y != -1 && obj_type == "Head") {
                results.push_back({obj_id, obj_type, x, y, likelihood, w, h});

                constexpr bool k_enable_object_log = true;
                if (k_enable_object_log) {
                    std::cout << "🎯 [OBJ] ID: " << obj_id
                              << " | Type: " << obj_type
                              << " | Pos: (" << x << ", " << y << ")"
                              << " | Size: (" << w << "x" << h << ")"
                              << " | Lkhd: " << likelihood << std::endl;
                }
            }
            search_pos = obj_start + 1;
        }

        // 이벤트 파싱
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
                    std::string sv = message_block.substr(start, end - start);
                    if (sv == "true" || sv == "1") is_active = true;
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
                              << " | Gap: " << diff_sec << "s" << std::endl;
                } else {
                    std::cout << "✅ [EVENT] " << rule_name
                              << " Active | ID: " << triggered_id << std::endl;
                }
                gate_last_pass_time[rule_name] = last_timestamp;
            }
            search_pos = msg_end;
        }
    } catch (...) {}
    return results;
}

// ─────────────────────────────────────────────────────────────────────
// parseHumanObjectsForAnalytics
// camera_client / camera_RBF 용 — IoU 트래킹 제거, bbox만 반환
// DeepSORT 워커가 camera_RBF.cpp 메인루프에서 ID를 부여함
// ─────────────────────────────────────────────────────────────────────
std::vector<ParsedMetadataObject> XMLParser::parseHumanObjectsForAnalytics(
        const std::string& xml, bool detect_all) const {
    std::vector<ParsedMetadataObject> results;
    results.reserve(8);
    std::size_t search_pos = 0;

    while (true) {
        const std::size_t obj_start = xml.find("<tt:Object", search_pos);
        if (obj_start == std::string::npos) break;

        const std::size_t next_obj = xml.find("<tt:Object", obj_start + 1);
        const std::size_t obj_end  = (next_obj == std::string::npos) ? xml.size() : next_obj;
        ParsedMetadataObject object = {"", "", -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};

        // id
        const std::size_t id_pos = find_in_range(xml, "ObjectId=\"", obj_start, obj_end);
        if (id_pos != std::string::npos) {
            const std::size_t start = id_pos + 10;
            const std::size_t end   = xml.find("\"", start);
            if (end != std::string::npos && end < obj_end)
                object.id = xml.substr(start, end - start);
        }

        // type — <tt:Type Likelihood=...> 두 번째 태그 사용
        const std::size_t type_pos = find_in_range(xml, "<tt:Type Likelihood=", obj_start, obj_end);
        if (type_pos != std::string::npos) {
            const std::size_t start = xml.find(">", type_pos) + 1;
            const std::size_t end   = xml.find("</tt:Type>", start);
            if (end != std::string::npos && end < obj_end)
                object.type = xml.substr(start, end - start);
        }

        // x, y — CenterOfGravity 기준
        const std::size_t cog_pos = find_in_range(xml, "CenterOfGravity", obj_start, obj_end);
        const std::size_t x_pos   = (cog_pos != std::string::npos)
                                    ? find_in_range(xml, "x=\"", cog_pos, obj_end) : std::string::npos;
        const std::size_t y_pos   = (cog_pos != std::string::npos)
                                    ? find_in_range(xml, "y=\"", cog_pos, obj_end) : std::string::npos;

        const std::size_t left_pos   = find_in_range(xml, "left=\"",   obj_start, obj_end);
        const std::size_t right_pos  = find_in_range(xml, "right=\"",  obj_start, obj_end);
        const std::size_t top_pos    = find_in_range(xml, "top=\"",    obj_start, obj_end);
        const std::size_t bottom_pos = find_in_range(xml, "bottom=\"", obj_start, obj_end);

        const bool has_x      = parse_float_attr(xml, x_pos,      3, object.x);
        const bool has_y      = parse_float_attr(xml, y_pos,      3, object.y);
        const bool has_left   = parse_float_attr(xml, left_pos,   6, object.left);
        const bool has_right  = parse_float_attr(xml, right_pos,  7, object.right);
        const bool has_top    = parse_float_attr(xml, top_pos,    5, object.top);
        const bool has_bottom = parse_float_attr(xml, bottom_pos, 8, object.bottom);

        //const bool type_ok = detect_all || (object.type == "Head");
        const bool type_ok = detect_all || (object.type == "Human");
        if (!object.id.empty() && type_ok && has_x && has_y &&
            has_left && has_right && has_top && has_bottom) {
            results.push_back(object);
        }

        search_pos = obj_end;
    }
    return results;
}