#include "XMLParser.h"
#include "Config.h"
#include "string"
#include <iostream>
#include <time.h>
#include <algorithm>

const std::string findObject = "Human";
constexpr bool k_enable_object_log = true;
constexpr bool k_enable_meta_log = false;

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

float calculate_iou(float l1, float t1, float r1, float b1,
                    float l2, float t2, float r2, float b2) {
    float xA = std::max(l1, l2);
    float yA = std::max(t1, t2);
    float xB = std::min(r1, r2);
    float yB = std::min(b1, b2);
    float interArea = std::max(0.0f, xB - xA) * std::max(0.0f, yB - yA);
    if (interArea == 0.0f) return 0.0f;
    float box1Area = (r1 - l1) * (b1 - t1);
    float box2Area = (r2 - l2) * (b2 - t2);
    return interArea / (box1Area + box2Area - interArea);
}

// ── NMS: likelihood 높은 순으로 정렬 후 IoU > thresh 인 박스 제거 ──────────
std::vector<ParsedMetadataObject> apply_nms(
    std::vector<ParsedMetadataObject> objs,
    float iou_thresh = 0.45f)
{
    if (objs.size() <= 1) return objs;

    auto box_area = [](const ParsedMetadataObject& o) {
        const float w = o.right - o.left;
        const float h = o.bottom - o.top;
        return (w > 0.0f && h > 0.0f) ? (w * h) : 0.0f;
    };
    std::sort(objs.begin(), objs.end(),
        [&box_area](const ParsedMetadataObject& a, const ParsedMetadataObject& b) {
            if (a.likelihood != b.likelihood)
                return a.likelihood > b.likelihood;
            return box_area(a) > box_area(b);
        });

    std::vector<bool> suppressed(objs.size(), false);
    for (std::size_t i = 0; i < objs.size(); ++i) {
        if (suppressed[i]) continue;
        for (std::size_t j = i + 1; j < objs.size(); ++j) {
            if (suppressed[j]) continue;
            float iou = calculate_iou(
                objs[i].left, objs[i].top, objs[i].right, objs[i].bottom,
                objs[j].left, objs[j].top, objs[j].right, objs[j].bottom);
            if (iou > iou_thresh)
                suppressed[j] = true;
        }
    }

    std::vector<ParsedMetadataObject> result;
    result.reserve(objs.size());
    for (std::size_t i = 0; i < objs.size(); ++i)
        if (!suppressed[i]) result.push_back(objs[i]);
    return result;
}

// inner 박스 면적 중 outer와 겹치는 비율이 thresh 이상이면(거의 안에 있음), inner 를 내부 중복으로 본다.
float inner_overlap_ratio_in_outer(const ParsedMetadataObject& inner,
                                   const ParsedMetadataObject& outer) {
    const float il = inner.left, it = inner.top, ir = inner.right, ib = inner.bottom;
    const float ol = outer.left, ot = outer.top, or_ = outer.right, ob = outer.bottom;
    const float xA = std::max(il, ol);
    const float yA = std::max(it, ot);
    const float xB = std::min(ir, or_);
    const float yB = std::min(ib, ob);
    const float inter = std::max(0.0f, xB - xA) * std::max(0.0f, yB - yA);
    const float inner_area = (ir - il) * (ib - it);
    if (inner_area <= 0.0f) return 0.0f;
    return inter / inner_area;
}

static float parsed_object_box_area(const ParsedMetadataObject& o) {
    const float w = o.right - o.left;
    const float h = o.bottom - o.top;
    return (w > 0.0f && h > 0.0f) ? (w * h) : 0.0f;
}

// 면적 큰 박스를 먼저 유지. 더 작은 박스가 기존 박스 안에 거의 통째로 들어가면 타입 무관하게 제거
// (ONVIF 가 머리만 잡아도 타입을 Human 으로 보내는 경우 대비)
std::vector<ParsedMetadataObject> remove_enclosed_smaller_boxes(
    std::vector<ParsedMetadataObject> objs,
    float cover_ratio_thresh = 0.82f,
    float max_inner_vs_outer_area = 0.98f)
{
    if (objs.size() <= 1) return objs;

    std::sort(objs.begin(), objs.end(),
        [](const ParsedMetadataObject& a, const ParsedMetadataObject& b) {
            return parsed_object_box_area(a) > parsed_object_box_area(b);
        });

    std::vector<ParsedMetadataObject> kept;
    kept.reserve(objs.size());
    for (const auto& c : objs) {
        const float ac = parsed_object_box_area(c);
        bool enclosed_in_kept = false;
        for (const auto& k : kept) {
            const float ak = parsed_object_box_area(k);
            if (ac >= ak * max_inner_vs_outer_area)
                continue;
            if (inner_overlap_ratio_in_outer(c, k) >= cover_ratio_thresh) {
                enclosed_in_kept = true;
                break;
            }
        }
        if (!enclosed_in_kept)
            kept.push_back(c);
    }
    return kept;
}

} // namespace

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
                size_t end_left   = accumulated_xml.find("\"", left_pos   + 6);
                size_t end_right  = accumulated_xml.find("\"", right_pos  + 7);
                size_t end_top    = accumulated_xml.find("\"", top_pos    + 5);
                size_t end_bottom = accumulated_xml.find("\"", bottom_pos + 8);

                left   = std::stof(accumulated_xml.substr(left_pos   + 6, end_left   - (left_pos   + 6)));
                right  = std::stof(accumulated_xml.substr(right_pos  + 7, end_right  - (right_pos  + 7)));
                top    = std::stof(accumulated_xml.substr(top_pos    + 5, end_top    - (top_pos    + 5)));
                bottom = std::stof(accumulated_xml.substr(bottom_pos + 8, end_bottom - (bottom_pos + 8)));

                w = right - left;
                h = bottom - top;
            }

            std::string real_id = obj_id;

            if (x != -1 && y != -1 && obj_type == findObject) {

                if (tracking_map.find(obj_id) == tracking_map.end()) {
                    std::string matched_old_id = "";
                    float max_iou = 0.05f;

                    for (auto& pair : tracking_map) {
                        const std::string& old_id = pair.first;
                        auto& track = pair.second;

                        unsigned int dt = last_timestamp - track.last_rtp;
                        if (dt > 0 && dt < 225000) {
                            float expected_x = track.last_x + (track.vx * dt);
                            float expected_y = track.last_y + (track.vy * dt);
                            expected_x = std::max(0.0f, std::min(expected_x, kParserClampWidth));
                            expected_y = std::max(0.0f, std::min(expected_y, kParserClampHeight));
                            float pred_l = expected_x - (track.last_w / 2.0f);
                            float pred_r = expected_x + (track.last_w / 2.0f);
                            float pred_t = expected_y - (track.last_h / 2.0f);
                            float pred_b = expected_y + (track.last_h / 2.0f);

                            float iou = calculate_iou(left, top, right, bottom, pred_l, pred_t, pred_r, pred_b);
                            float threshold = (likelihood < 0.5f) ? 0.01f : 0.05f;

                            if (iou > threshold && iou > max_iou) {
                                max_iou = iou;
                                matched_old_id = old_id;
                            }
                        }
                    }

                    if (matched_old_id != "") {

                        tracking_map[obj_id] = tracking_map[matched_old_id]; 
                        tracking_map.erase(matched_old_id); 
                        real_id = tracking_map[obj_id].original_id; 
                        tracking_map[obj_id].last_w = w;
                        tracking_map[obj_id].last_h = h;
                        std::cout << "🔗 [ID 복구] 카메라 ID: " << obj_id
                                  << " -> 오리지널 ID: " << real_id
                                  << " (IoU 매칭률: " << (int)(max_iou * 100) << "%)" << std::endl;
                    } else {
                        tracking_map[obj_id] = {obj_id, x, y, w, h, 0.0f, 0.0f, last_timestamp};
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
                    track.last_w = w;
                    track.last_h = h;
                    track.last_rtp = last_timestamp;

                    real_id = track.original_id;
                }

                results.push_back({real_id, obj_type, x, y, likelihood, w, h});

                bool is_new_id = (log_timer_map.find(real_id) == log_timer_map.end());
                if (k_enable_object_log && (is_new_id || (last_timestamp - log_timer_map[real_id] > kParserObjectLogIntervalRtp))) {
                    std::string prefix = is_new_id ? "✨ [NEW]" : "🎯 [OBJ]";
                    std::cout << prefix << " ID: " << real_id
                              << " | Type: " << obj_type
                              << " | Pos: (" << x << ", " << y << ")"
                              << " | Size: (" << w << "x" << h << ")"
                              << " | Lkhd: " << likelihood
                              << " | TagTime: " << tag_time << std::endl;
                    log_timer_map[real_id] = last_timestamp;
                }
            }
            search_pos = obj_start + 1;
        }

        for (auto it = tracking_map.begin(); it != tracking_map.end(); ) {
            if (last_timestamp - it->second.last_rtp > 450000)
                it = tracking_map.erase(it);
            else
                ++it;
        }

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
                if (time_diff < kParserTailgateGapRtp && gate_last_pass_time[rule_name] != 0) {
                    float diff_sec = (float)time_diff / 90000.0f;
                    std::cout << "🚨 [TAILGATING] " << rule_name 
                              << " | Trigger ID: " << triggered_id
                              << " | RTP: " << last_timestamp 
                              << " | Gap: " << diff_sec << "s" << std::endl;
                } else {
                    std::cout << "🎯 [EVENT] " << rule_name
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

std::vector<ParsedMetadataObject> XMLParser::parseHumanObjectsForAnalytics(const std::string& xml,
                                                                           bool detect_all) const {
    std::vector<ParsedMetadataObject> all_with_geometry;
    all_with_geometry.reserve(8);
    std::size_t search_pos = 0;

    while (true) {
        const std::size_t obj_start = xml.find("<tt:Object", search_pos);
        if (obj_start == std::string::npos) break;

        const std::size_t next_obj = xml.find("<tt:Object", obj_start + 1);
        const std::size_t obj_end = (next_obj == std::string::npos) ? xml.size() : next_obj;
        ParsedMetadataObject object = {"", "", -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 1.0f};

        const std::size_t id_pos = find_in_range(xml, "ObjectId=\"", obj_start, obj_end);
        if (id_pos != std::string::npos) {
            const std::size_t start = id_pos + 10;
            const std::size_t end = xml.find("\"", start);
            if (end != std::string::npos && end < obj_end)
                object.id = xml.substr(start, end - start);
        }

        const std::size_t type_pos = find_in_range(xml, "<tt:Type Likelihood=", obj_start, obj_end);
        if (type_pos != std::string::npos) {
            const std::size_t start = xml.find(">", type_pos) + 1;
            const std::size_t end = xml.find("</tt:Type>", start);
            if (end != std::string::npos && end < obj_end)
                object.type = xml.substr(start, end - start);
        }

        const std::size_t likelihood_pos = find_in_range(xml, "<tt:Likelihood>", obj_start, obj_end);
        if (likelihood_pos != std::string::npos) {
            const std::size_t start = likelihood_pos + 15;
            const std::size_t end = xml.find("</tt:Likelihood>", start);
            if (end != std::string::npos && end < obj_end) {
                try {
                    object.likelihood = std::stof(xml.substr(start, end - start));
                } catch (...) {
                    object.likelihood = 1.0f;
                }
            }
        }

        const std::size_t cog_pos = find_in_range(xml, "CenterOfGravity", obj_start, obj_end);
        const std::size_t x_pos = (cog_pos != std::string::npos)
                                  ? find_in_range(xml, "x=\"", cog_pos, obj_end) : std::string::npos;
        const std::size_t y_pos = (cog_pos != std::string::npos)
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

        const bool type_ok = detect_all || (object.type == findObject);
        const bool geom_ok = has_x && has_y && has_left && has_right && has_top && has_bottom;
        // 타입 필터 전에 전부 모아야 Human-only 모드에서도 (타입이 Human 인) 소박스가 전신 박스 판단에 참여함
        if (!object.id.empty() && geom_ok) {
            all_with_geometry.push_back(object);
            if (k_enable_meta_log && type_ok) {
                std::cout << "[META] id=" << object.id
                          << " type=" << object.type
                          << " x=" << object.x
                          << " y=" << object.y
                          << " left=" << object.left
                          << " right=" << object.right
                          << " top=" << object.top
                          << " bottom=" << object.bottom
                          << std::endl;
            }
        }

        search_pos = obj_end;
    }

    // 1) 더 큰 박스 안에 거의 통째로 들어간 작은 박스 제거(타입 무관)
    // 2) 같은 스케일 중복은 NMS
    constexpr float k_human_nms_iou = 0.35f;
    auto no_nested = remove_enclosed_smaller_boxes(std::move(all_with_geometry));
    auto nmsed = apply_nms(std::move(no_nested), k_human_nms_iou);

    std::vector<ParsedMetadataObject> results;
    results.reserve(nmsed.size());
    for (const auto& o : nmsed) {
        if (detect_all || o.type == findObject)
            results.push_back(o);
    }
    return results;
}
