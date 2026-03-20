#include "analytics.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

#include "alert.h"
#include "env_utils.h"
#include "sample_utils.h"
#include "text_utils.h"

namespace {
constexpr const char* kAnalyticsInsertQuery =
    "INSERT INTO analytics_logs (object_id, card_age_text, age, is_fraud, created_at) "
    "VALUES (?, ?, ?, ?, NOW())";
constexpr const char* kAnalyticsLogPrefix = "[analytics.cpp]";

struct ParsedEventForAnalytics {
    std::string rule_name;
    std::string object_id;
    std::string tag_time;
    bool is_active = false;
};

std::string extract_tag_time(const std::string& raw) {
    constexpr const char* kUnknownTagTime = "Unknown";
    const std::size_t utc_pos = raw.find("UtcTime=\"");
    if (utc_pos == std::string::npos) return kUnknownTagTime;

    const std::size_t start = utc_pos + 9;
    const std::size_t end = raw.find("\"", start);
    if (end == std::string::npos) return kUnknownTagTime;
    return raw.substr(start, end - start);
}

std::vector<ParsedEventForAnalytics> parse_events_for_analytics(const std::string& raw,
                                                                const std::string& tag_time) {
    std::vector<ParsedEventForAnalytics> parsed_events;
    parsed_events.reserve(4);

    std::size_t search_pos = 0;
    while (true) {
        const std::size_t msg_start = raw.find("<wsnt:NotificationMessage", search_pos);
        if (msg_start == std::string::npos) break;

        const std::size_t msg_end = raw.find("</wsnt:NotificationMessage>", msg_start);
        if (msg_end == std::string::npos) break;

        const std::string message_block = raw.substr(msg_start, msg_end - msg_start);
        ParsedEventForAnalytics event;
        event.tag_time = tag_time;

        const std::size_t name_item_pos = message_block.find("Name=\"RuleName\"");
        if (name_item_pos != std::string::npos) {
            const std::size_t val_pos = message_block.find("Value=\"", name_item_pos);
            if (val_pos != std::string::npos) {
                const std::size_t start = val_pos + 7;
                const std::size_t end = message_block.find("\"", start);
                if (end != std::string::npos) {
                    event.rule_name = message_block.substr(start, end - start);
                }
            }
        }

        const std::size_t state_item_pos = message_block.find("Name=\"State\"");
        if (state_item_pos != std::string::npos) {
            const std::size_t val_pos = message_block.find("Value=\"", state_item_pos);
            if (val_pos != std::string::npos) {
                const std::size_t start = val_pos + 7;
                const std::size_t end = message_block.find("\"", start);
                if (end != std::string::npos) {
                    const std::string state_val = message_block.substr(start, end - start);
                    event.is_active = (state_val == "true" || state_val == "1");
                }
            }
        }

        const std::size_t id_item_pos = message_block.find("Name=\"ObjectId\"");
        if (id_item_pos != std::string::npos) {
            const std::size_t val_pos = message_block.find("Value=\"", id_item_pos);
            if (val_pos != std::string::npos) {
                const std::size_t start = val_pos + 7;
                const std::size_t end = message_block.find("\"", start);
                if (end != std::string::npos) {
                    event.object_id = message_block.substr(start, end - start);
                }
            }
        }

        if (!event.rule_name.empty()) {
            parsed_events.push_back(std::move(event));
        }
        search_pos = msg_end;
    }

    return parsed_events;
}

struct CardAgeDecision {
    const char* canonical_text;
    int bucket;
    bool known;
};

enum AgeBucket {
    kAgeBucketUnknown = -1,
    kAgeBucketYouth = 0,
    kAgeBucketAdult = 1,
    kAgeBucketSenior = 2
};

int extract_first_number(std::string_view raw) {
    std::size_t i = 0;
    while (i < raw.size() && std::isdigit(static_cast<unsigned char>(raw[i])) == 0) ++i;
    if (i >= raw.size()) return -1;
    int value = 0;
    while (i < raw.size() && std::isdigit(static_cast<unsigned char>(raw[i])) != 0) {
        value = (value * 10) + (raw[i] - '0');
        ++i;
    }
    return value;
}

int age_bucket_from_age(std::string_view raw) {
    const std::string lower = to_lower_copy(raw);
    if (lower.find("youth") != std::string::npos) return kAgeBucketYouth;
    if (lower.find("adult") != std::string::npos) return kAgeBucketAdult;
    if (lower.find("senior") != std::string::npos) return kAgeBucketSenior;

    const int age_number = extract_first_number(lower);
    if (age_number < 0) return kAgeBucketUnknown;

    if (age_number < 20) return kAgeBucketYouth;
    if (age_number < 60) return kAgeBucketAdult;
    return kAgeBucketSenior;
}

CardAgeDecision evaluate_card_age(std::string_view raw) {
    const std::string normalized = to_lower_copy(trim_copy(std::string(raw)));

    if (normalized == "adult") return {"Adult", kAgeBucketAdult, true};
    if (normalized == "senior") return {"Senior", kAgeBucketSenior, true};
    if (normalized == "youth") return {"Youth", kAgeBucketYouth, true};
    return {"0", kAgeBucketUnknown, false};
}

bool is_fraud_by_age_mismatch(const CardAgeDecision& card_age, std::string_view age) {
    const int camera_bucket = age_bucket_from_age(age);
    if (!card_age.known || camera_bucket == kAgeBucketUnknown) {
        return true;
    }
    return card_age.bucket != camera_bucket;
}

std::string fraud_flag(bool is_fraud) {
    return is_fraud ? "Y" : "N";
}

std::string normalize_rule_name(std::string_view raw) {
    return to_lower_copy(trim_copy(std::string(raw)));
}
}  // namespace

AnalyticsProcessor::AnalyticsProcessor(const char* h,
                                       const char* u,
                                       const char* p,
                                       const char* d)
    : host(h ? h : ""),
      user(u ? u : ""),
      pass(p ? p : ""),
      db(d ? d : ""),
      conn(nullptr),
      analyticsInsertStmt(nullptr),
      running(false),
      max_lines_per_batch(
          load_env_size_t("SFEPS_META_MAX_LINES_PER_BATCH", 128, 1, kAnalyticsLogPrefix)),
      max_queue_size(load_env_size_t("SFEPS_ANALYTICS_QUEUE_MAX", 200, 1, kAnalyticsLogPrefix)),
      max_pending_size(load_env_size_t("SFEPS_META_PENDING_MAX", 2048, 1, kAnalyticsLogPrefix)),
      pending_ttl_seconds(
          load_env_size_t("SFEPS_META_PENDING_TTL_SEC", 30, 1, kAnalyticsLogPrefix)),
      drop_log_interval(load_env_size_t("SFEPS_DROP_LOG_INTERVAL", 100, 1, kAnalyticsLogPrefix)),
      enter_rule_name(normalize_rule_name(load_env_string("SFEPS_META_ENTER_RULE", "enterline"))),
      outline_rule_name(normalize_rule_name(load_env_string("SFEPS_META_OUTLINE_RULE", "outline"))),
      dropped_line_limit_count(0),
      dropped_queue_count(0),
      dropped_pending_expired_count(0),
      dropped_pending_overflow_count(0),
      parsed_xml_ok_count(0) {}

AnalyticsProcessor::~AnalyticsProcessor() {
    stop();
}

bool AnalyticsProcessor::prepareStatements() {
    if (conn == nullptr) return false;

    analyticsInsertStmt = mysql_stmt_init(conn);
    if (analyticsInsertStmt == nullptr) {
        std::cerr << "[Analytics DB Error] analytics_logs insert용 mysql_stmt_init() 실패"
                  << std::endl;
        return false;
    }

    if (mysql_stmt_prepare(analyticsInsertStmt, kAnalyticsInsertQuery,
                           std::strlen(kAnalyticsInsertQuery)) != 0) {
        std::cerr << "[Analytics DB Error] prepare 실패: "
                  << mysql_stmt_error(analyticsInsertStmt) << std::endl;
        mysql_stmt_close(analyticsInsertStmt);
        analyticsInsertStmt = nullptr;
        return false;
    }

    return true;
}

void AnalyticsProcessor::closeStatements() {
    if (analyticsInsertStmt != nullptr) {
        mysql_stmt_close(analyticsInsertStmt);
        analyticsInsertStmt = nullptr;
    }
}

bool AnalyticsProcessor::start() {
    if (running.load()) return true;

    conn = mysql_init(nullptr);
    if (conn == nullptr) {
        std::cerr << "[Analytics] mysql_init 실패." << std::endl;
        return false;
    }

    if (mysql_real_connect(conn, host.c_str(), user.c_str(), pass.c_str(), db.c_str(), 0, nullptr, 0) ==
        nullptr) {
        std::cerr << "[Analytics] DB 연결 오류: " << mysql_error(conn) << std::endl;
        mysql_close(conn);
        conn = nullptr;
        return false;
    }

    if (!prepareStatements()) {
        closeStatements();
        mysql_close(conn);
        conn = nullptr;
        return false;
    }

    running = true;
    worker = std::thread(&AnalyticsProcessor::workerLoop, this);
    std::cout << "[analytics.cpp] [Analytics] 시작됨." << std::endl;
    return true;
}

void AnalyticsProcessor::stop() {
    const bool was_running = running.exchange(false);
    cv.notify_all();

    if (worker.joinable()) {
        worker.join();
    }

    if (!was_running && conn == nullptr && analyticsInsertStmt == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mtx);
        pending_queue.clear();
        pending_object_ids.clear();
        matched_objects.clear();
        latest_objects.clear();
        object_fraud_flags.clear();
        while (!q.empty()) q.pop();
    }

    closeStatements();
    if (conn != nullptr) {
        mysql_close(conn);
        conn = nullptr;
    }
}

void AnalyticsProcessor::setFraudBBoxCallback(FraudBBoxCallback callback) {
    fraud_bbox_callback = std::move(callback);
}

void AnalyticsProcessor::setRfidPairedCallback(RfidPairedCallback callback) {
    rfid_paired_callback = std::move(callback);
}

void AnalyticsProcessor::setOutlineDecisionCallback(OutlineDecisionCallback callback) {
    outline_decision_callback = std::move(callback);
}

bool AnalyticsProcessor::getObjectPositionSnapshot(const std::string& object_id,
                                                   ObjectPositionSnapshot& out) const {
    constexpr std::size_t kMaxObjectIdBytes = 128;

    std::string key = trim_copy(object_id);
    if (key.empty()) return false;
    if (key.size() > kMaxObjectIdBytes) {
        key.resize(kMaxObjectIdBytes);
    }

    std::lock_guard<std::mutex> lock(mtx);
    const auto it = latest_objects.find(key);
    if (it == latest_objects.end()) return false;

    out.object_id = key;
    out.left = it->second.left;
    out.top = it->second.top;
    out.right = it->second.right;
    out.bottom = it->second.bottom;
    out.x = it->second.x;
    out.y = it->second.y;
    const auto fraud_it = object_fraud_flags.find(key);
    out.is_fraud = (fraud_it != object_fraud_flags.end()) ? fraud_it->second : false;
    out.tag_time = it->second.tag_time;
    out.updated_at = it->second.updated_at;
    return true;
}

void AnalyticsProcessor::getAllObjectSnapshots(std::vector<ObjectPositionSnapshot>& out) const {
    std::lock_guard<std::mutex> lock(mtx);
    out.clear();
    out.reserve(latest_objects.size());
    for (const auto& entry : latest_objects) {
        const std::string& object_id = entry.first;
        const LatestObjectInfo& info = entry.second;

        ObjectPositionSnapshot snapshot;
        snapshot.object_id = object_id;
        snapshot.left = info.left;
        snapshot.top = info.top;
        snapshot.right = info.right;
        snapshot.bottom = info.bottom;
        snapshot.x = info.x;
        snapshot.y = info.y;
        const auto fraud_it = object_fraud_flags.find(object_id);
        snapshot.is_fraud = (fraud_it != object_fraud_flags.end()) ? fraud_it->second : false;
        snapshot.tag_time = info.tag_time;
        snapshot.updated_at = info.updated_at;
        out.push_back(std::move(snapshot));
    }
}

void AnalyticsProcessor::pruneExpiredPendingLocked(std::chrono::steady_clock::time_point now) {
    const auto ttl = std::chrono::seconds(static_cast<long long>(pending_ttl_seconds));
    std::uint64_t expired = 0;

    while (!pending_queue.empty()) {
        const auto& front = pending_queue.front();
        if ((now - front.created_at) <= ttl) break;
        pending_object_ids.erase(front.object_id);
        pending_queue.pop_front();
        ++expired;
    }

    if (expired == 0) return;

    const std::uint64_t total_expired = dropped_pending_expired_count.fetch_add(expired) + expired;
    if (should_sample(total_expired, drop_log_interval)) {
        std::cout << "[analytics.cpp] [Drop] 만료된 pending object id 제거: 만료 수=" << expired
                  << ", total_expired=" << total_expired
                  << ", pending_remaining=" << pending_queue.size() << std::endl;
    }
}

void AnalyticsProcessor::pruneExpiredStateLocked(std::chrono::steady_clock::time_point now) {
    const auto ttl = std::chrono::seconds(static_cast<long long>(pending_ttl_seconds));

    for (auto it = matched_objects.begin(); it != matched_objects.end();) {
        if ((now - it->second.created_at) > ttl) {
            it = matched_objects.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = latest_objects.begin(); it != latest_objects.end();) {
        if ((now - it->second.updated_at) > ttl) {
            object_fraud_flags.erase(it->first);
            it = latest_objects.erase(it);
        } else {
            ++it;
        }
    }
}

void AnalyticsProcessor::publishRaw(const std::string& raw) {
    if (!running.load()) return;
    if (raw.empty()) return;

    constexpr std::size_t kMaxObjectIdBytes = 128;

    // Values parsed by Camera/get_metadata(XMLParser):
    // id, type, x, y, left, top, right, bottom from <tt:Object>.
    const std::vector<ParsedMetadataObject> parsed_objects =
        xml_parser.parseHumanObjectsForAnalytics(raw);
    // Event fields are parsed here in server from NotificationMessage:
    // RuleName, State, ObjectId, and frame UtcTime(tag_time).
    const std::string tag_time = extract_tag_time(raw);
    const std::vector<ParsedEventForAnalytics> parsed_events =
        parse_events_for_analytics(raw, tag_time);

    ++parsed_xml_ok_count;

    if (parsed_objects.empty() && parsed_events.empty()) return;

    std::size_t accepted_enter_count = 0;
    bool line_limit_hit = false;
    bool should_notify_worker = false;
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> outbound_alerts;
    std::vector<FraudBBoxPayload> outbound_esp_bbox;
    std::vector<OutlineDecisionPayload> outbound_outline_decisions;

    {
        std::lock_guard<std::mutex> lock(mtx);
        pruneExpiredPendingLocked(now);
        pruneExpiredStateLocked(now);

        // Consume object values that already came from Camera/get_metadata parser.
        for (const auto& human_object : parsed_objects) {
            std::string object_id = trim_copy(human_object.id);
            if (object_id.empty()) continue;
            if (object_id.size() > kMaxObjectIdBytes) {
                object_id.resize(kMaxObjectIdBytes);
            }

            LatestObjectInfo info;
            info.x = human_object.x;
            info.y = human_object.y;
            info.left = human_object.left;
            info.top = human_object.top;
            info.right = human_object.right;
            info.bottom = human_object.bottom;
            info.tag_time = tag_time;
            info.updated_at = now;
            latest_objects[object_id] = std::move(info);
            if (object_fraud_flags.find(object_id) == object_fraud_flags.end()) {
                object_fraud_flags[object_id] = false;
            }
        }

        for (const auto& event : parsed_events) {
            if (!event.is_active) continue;

            const std::string rule_name = normalize_rule_name(event.rule_name);
            std::string object_id = trim_copy(event.object_id);
            if (object_id.empty()) continue;
            if (object_id.size() > kMaxObjectIdBytes) {
                object_id.resize(kMaxObjectIdBytes);
            }

            const bool is_outline_rule = (rule_name == outline_rule_name);

            if (rule_name == enter_rule_name) {
                if (accepted_enter_count >= max_lines_per_batch) {
                    line_limit_hit = true;
                    continue;
                }
                if (pending_object_ids.find(object_id) != pending_object_ids.end()) continue;
                if (matched_objects.find(object_id) != matched_objects.end()) continue;

                if (pending_queue.size() >= max_pending_size) {
                    const std::string dropped_id = pending_queue.front().object_id;
                    pending_queue.pop_front();
                    pending_object_ids.erase(dropped_id);

                    const std::uint64_t dropped = ++dropped_pending_overflow_count;
                    if (should_sample(dropped, drop_log_interval)) {
                        std::cout << "[analytics.cpp] [Drop] pending object 큐 초과: 최대="
                                  << max_pending_size << ", dropped_count=" << dropped << std::endl;
                    }
                }

                PendingObject pending;
                pending.object_id = object_id;
                pending.card_age_text = "0";
                pending.age = "20";
                pending.enter_tag_time = event.tag_time;
                pending.is_fraud = true;
                pending.created_at = now;

                const auto latest_it = latest_objects.find(object_id);
                if (latest_it != latest_objects.end()) {
                    pending.center_x = latest_it->second.x;
                    pending.center_y = latest_it->second.y;
                    pending.bbox_left = latest_it->second.left;
                    pending.bbox_top = latest_it->second.top;
                    pending.bbox_right = latest_it->second.right;
                    pending.bbox_bottom = latest_it->second.bottom;
                }

                pending_queue.push_back(std::move(pending));
                pending_object_ids.insert(object_id);
                ++accepted_enter_count;
                continue;
            }

            if (!is_outline_rule) continue;

            PendingObject final_out;
            bool found = false;

            auto matched_it = matched_objects.find(object_id);
            if (matched_it != matched_objects.end()) {
                final_out = matched_it->second;
                matched_objects.erase(matched_it);
                found = true;
            } else {
                for (auto it = pending_queue.begin(); it != pending_queue.end(); ++it) {
                    if (it->object_id == object_id) {
                        final_out = *it;
                        pending_queue.erase(it);
                        pending_object_ids.erase(object_id);
                        found = true;
                        break;
                    }
                }
            }

            if (!found) {
                final_out.object_id = object_id;
                final_out.card_age_text = "0";
                final_out.age = "20";
                final_out.is_fraud = true;
                final_out.created_at = now;
            }

            final_out.outline_tag_time = event.tag_time;
            const auto latest_it = latest_objects.find(object_id);
            if (latest_it != latest_objects.end()) {
                final_out.center_x = latest_it->second.x;
                final_out.center_y = latest_it->second.y;
                final_out.bbox_left = latest_it->second.left;
                final_out.bbox_top = latest_it->second.top;
                final_out.bbox_right = latest_it->second.right;
                final_out.bbox_bottom = latest_it->second.bottom;
            }

            if (final_out.card_age_text.empty()) {
                final_out.card_age_text = "0";
            }
            const CardAgeDecision card_age = evaluate_card_age(final_out.card_age_text);
            final_out.is_fraud = is_fraud_by_age_mismatch(card_age, final_out.age);
            object_fraud_flags[final_out.object_id] = final_out.is_fraud;

            FraudRecord record;
            record.object_id = final_out.object_id;
            record.card_age_text = final_out.card_age_text;
            record.age = final_out.age;
            record.is_fraud = final_out.is_fraud;

            if (q.size() >= max_queue_size) {
                q.pop();
                const std::uint64_t dropped = ++dropped_queue_count;
                if (should_sample(dropped, drop_log_interval)) {
                    std::cout << "[analytics.cpp] [Drop] analytics 큐 초과: 최대="
                              << max_queue_size << ", dropped_count=" << dropped << std::endl;
                }
            }
            q.push(record);
            should_notify_worker = true;

            const std::string fraud_yn = fraud_flag(final_out.is_fraud);
            char merged_line[512];
            const int n = std::snprintf(
                merged_line, sizeof(merged_line),
                "FRAUD|%s|%s|%s|%s|L=%.1f|T=%.1f|R=%.1f|B=%.1f|X=%.1f|Y=%.1f|TAG=%s\n",
                record.object_id.c_str(), record.card_age_text.c_str(), record.age.c_str(),
                fraud_yn.c_str(), final_out.bbox_left, final_out.bbox_top, final_out.bbox_right,
                final_out.bbox_bottom, final_out.center_x, final_out.center_y,
                final_out.outline_tag_time.c_str());
            if (n > 0 && n < static_cast<int>(sizeof(merged_line))) {
                outbound_alerts.emplace_back(merged_line, static_cast<std::size_t>(n));
            }

            OutlineDecisionPayload outline_payload;
            outline_payload.object_id = final_out.object_id;
            outline_payload.card_age_text = final_out.card_age_text;
            outline_payload.age = final_out.age;
            outline_payload.is_fraud = final_out.is_fraud;
            outline_payload.tag_time = final_out.outline_tag_time;
            outbound_outline_decisions.push_back(std::move(outline_payload));

            if (final_out.is_fraud &&
                final_out.bbox_left >= 0.0f && final_out.bbox_top >= 0.0f &&
                final_out.bbox_right >= final_out.bbox_left &&
                final_out.bbox_bottom >= final_out.bbox_top) {
                FraudBBoxPayload bbox_payload;
                bbox_payload.object_id = final_out.object_id;
                bbox_payload.card_age_text = final_out.card_age_text;
                bbox_payload.age = final_out.age;
                bbox_payload.left = final_out.bbox_left;
                bbox_payload.top = final_out.bbox_top;
                bbox_payload.right = final_out.bbox_right;
                bbox_payload.bottom = final_out.bbox_bottom;
                outbound_esp_bbox.push_back(std::move(bbox_payload));
            }
        }
    }

    if (line_limit_hit) {
        const std::uint64_t dropped = ++dropped_line_limit_count;
        if (should_sample(dropped, drop_log_interval)) {
            std::cout << "[analytics.cpp] [Drop] human object id 제한 초과: 최대="
                      << max_lines_per_batch << ", dropped_count=" << dropped << std::endl;
        }
    }

    for (const auto& msg : outbound_alerts) {
        send_alert_to_clients(msg);
    }
    if (fraud_bbox_callback) {
        for (const auto& payload : outbound_esp_bbox) {
            fraud_bbox_callback(payload);
        }
    }
    if (outline_decision_callback) {
        for (const auto& payload : outbound_outline_decisions) {
            outline_decision_callback(payload);
        }
    }
    if (should_notify_worker) {
        cv.notify_one();
    }
}

void AnalyticsProcessor::onRfidRead(const std::string& card_age_text_raw) {
    if (!running.load()) return;

    const CardAgeDecision card_age = evaluate_card_age(card_age_text_raw);
    const auto now = std::chrono::steady_clock::now();
    std::string paired_object_id;

    {
        std::lock_guard<std::mutex> lock(mtx);
        pruneExpiredPendingLocked(now);
        pruneExpiredStateLocked(now);

        if (pending_queue.empty()) {
            static std::uint64_t no_pending_count = 0;
            ++no_pending_count;
            if (should_sample(no_pending_count, drop_log_interval)) {
                std::cout << "[analytics.cpp] [Matcher] RFID 읽기 무시: pending object 없음"
                          << std::endl;
            }
            return;
        }

        PendingObject pending = std::move(pending_queue.front());
        pending_queue.pop_front();
        pending_object_ids.erase(pending.object_id);

        pending.card_age_text = card_age.canonical_text;
        pending.is_fraud = is_fraud_by_age_mismatch(card_age, pending.age);
        paired_object_id = pending.object_id;
        matched_objects[pending.object_id] = std::move(pending);
    }

    static std::uint64_t paired_count = 0;
    ++paired_count;
    if (should_sample(paired_count, std::max<std::size_t>(1000, drop_log_interval))) {
        std::cout << "[analytics.cpp] [Matcher] RFID enterline 매칭됨: object_id="
                  << paired_object_id << ", card_age_text=" << card_age.canonical_text
                  << ", paired_count=" << paired_count << std::endl;
    }
    if (rfid_paired_callback && !paired_object_id.empty()) {
        rfid_paired_callback(paired_object_id);
    }
}

bool AnalyticsProcessor::insertAnalyticsRow(const FraudRecord& record) {
    if (conn == nullptr || analyticsInsertStmt == nullptr) return false;

    if (mysql_stmt_reset(analyticsInsertStmt) != 0) {
        std::cerr << "[Analytics DB Error] stmt reset 실패: "
                  << mysql_stmt_error(analyticsInsertStmt) << std::endl;
        return false;
    }

    MYSQL_BIND params[4];
    std::memset(params, 0, sizeof(params));

    unsigned long object_id_len = static_cast<unsigned long>(record.object_id.size());
    unsigned long card_age_text_len = static_cast<unsigned long>(record.card_age_text.size());
    unsigned long age_len = static_cast<unsigned long>(record.age.size());
    signed char fraud_value = record.is_fraud ? 1 : 0;

    params[0].buffer_type = MYSQL_TYPE_STRING;
    params[0].buffer = const_cast<char*>(record.object_id.c_str());
    params[0].buffer_length = object_id_len;
    params[0].length = &object_id_len;

    params[1].buffer_type = MYSQL_TYPE_STRING;
    params[1].buffer = const_cast<char*>(record.card_age_text.c_str());
    params[1].buffer_length = card_age_text_len;
    params[1].length = &card_age_text_len;

    params[2].buffer_type = MYSQL_TYPE_STRING;
    params[2].buffer = const_cast<char*>(record.age.c_str());
    params[2].buffer_length = age_len;
    params[2].length = &age_len;

    params[3].buffer_type = MYSQL_TYPE_TINY;
    params[3].buffer = &fraud_value;
    params[3].is_unsigned = 0;

    if (mysql_stmt_bind_param(analyticsInsertStmt, params) != 0) {
        std::cerr << "[Analytics DB Error] stmt bind 실패: "
                  << mysql_stmt_error(analyticsInsertStmt) << std::endl;
        return false;
    }

    if (mysql_stmt_execute(analyticsInsertStmt) != 0) {
        std::cerr << "[Analytics DB Error] stmt execute 실패: "
                  << mysql_stmt_error(analyticsInsertStmt) << std::endl;
        return false;
    }

    return true;
}

void AnalyticsProcessor::workerLoop() {
    while (true) {
        std::vector<FraudRecord> batch;
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [&] { return !q.empty() || !running.load(); });
            if (!running.load() && q.empty()) {
                break;
            }

            while (!q.empty()) {
                batch.push_back(std::move(q.front()));
                q.pop();
            }
        }

        for (const auto& record : batch) {
            if (!record.is_fraud) continue;

            if (!insertAnalyticsRow(record)) {
                std::cerr << "[Analytics DB Error] fraud row insert 실패: object_id="
                          << record.object_id << ", card_age_text=" << record.card_age_text
                          << std::endl;
            }
        }
    }
}
