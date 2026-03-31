#include "analytics.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
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
constexpr const char* kAnalyticsLogPrefix = "analytics.cpp";
constexpr auto kOutlineRfidGracePeriod = std::chrono::seconds(1);

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

void print_section_log(const std::string& title, const std::vector<std::string>& lines) {
    std::cout << "[" << title << "]" << std::endl;
    for (const auto& line : lines) {
        std::cout << line << std::endl;
    }
    std::cout << "--------------------" << std::endl;
}

void log_enterline_trigger(const std::string& object_id) {
    print_section_log("진입 감지", {
        "ID   : " + object_id,
    });
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
    if (age_number < 65) return kAgeBucketAdult;
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
    if (!card_age.known) {
        return true;
    }

    const int age_number = extract_first_number(age);
    if (age_number >= 0) {
        if (age_number < 20) {
            return card_age.bucket != kAgeBucketYouth && card_age.bucket != kAgeBucketAdult;
        }
        if (age_number >= 65) {
            return card_age.bucket != kAgeBucketAdult && card_age.bucket != kAgeBucketSenior;
        }
    }

    const int camera_bucket = age_bucket_from_age(age);
    if (camera_bucket == kAgeBucketUnknown) {
        return true;
    }
    if (camera_bucket == kAgeBucketYouth) {
        return card_age.bucket != kAgeBucketYouth && card_age.bucket != kAgeBucketAdult;
    }
    if (camera_bucket == kAgeBucketSenior) {
        return card_age.bucket != kAgeBucketAdult && card_age.bucket != kAgeBucketSenior;
    }
    return card_age.bucket != kAgeBucketAdult;
}

std::string fraud_flag(bool is_fraud) {
    return is_fraud ? "Y" : "N";
}

std::string generate_weighted_random_age() {
    return "33";
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
      dropped_pending_overflow_count(0) {}

AnalyticsProcessor::~AnalyticsProcessor() {
    stop();
}

bool AnalyticsProcessor::prepareStatements() {
    if (conn == nullptr) return false;

    analyticsInsertStmt = mysql_stmt_init(conn);
    if (analyticsInsertStmt == nullptr) return false;

    if (mysql_stmt_prepare(analyticsInsertStmt, kAnalyticsInsertQuery,
                           std::strlen(kAnalyticsInsertQuery)) != 0) {
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
    if (conn == nullptr) return false;

    if (mysql_real_connect(conn, host.c_str(), user.c_str(), pass.c_str(), db.c_str(), 0, nullptr, 0) ==
        nullptr) {
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
        awaiting_outline_queue.clear();
        pending_object_ids.clear();
        matched_objects.clear();
        latest_objects.clear();
        object_fraud_flags.clear();
        bbox_aliases_by_event_id.clear();
        active_fraud_tracks.clear();
        while (!q.empty()) q.pop();
    }

    closeStatements();
    if (conn != nullptr) {
        mysql_close(conn);
        conn = nullptr;
    }
}

void AnalyticsProcessor::setTrackPosCallback(TrackPosCallback callback) {
    track_pos_callback = std::move(callback);
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

    dropped_pending_expired_count.fetch_add(expired);
}

void AnalyticsProcessor::finalizePendingObjectLocked(
    PendingObject& final_out,
    std::chrono::steady_clock::time_point now,
    std::vector<std::string>& outbound_alerts,
    std::vector<OutlineDecisionPayload>& outbound_outline_decisions,
    bool& should_notify_worker) {
    if (final_out.card_age_text.empty()) {
        final_out.card_age_text = "0";
    }
    const CardAgeDecision card_age = evaluate_card_age(final_out.card_age_text);
    final_out.is_fraud = is_fraud_by_age_mismatch(card_age, final_out.age);
    object_fraud_flags[final_out.object_id] = final_out.is_fraud;
    finalized_decisions[final_out.object_id] =
        FinalizedDecisionState {final_out.card_age_text, final_out.age, final_out.is_fraud, now};
    if (!final_out.source_object_id.empty()) {
        bbox_aliases_by_event_id[final_out.object_id] =
            EventBBoxAlias {final_out.source_object_id, now};
    }

    FraudRecord record;
    record.object_id = final_out.object_id;
    record.card_age_text = final_out.card_age_text;
    record.age = final_out.age;
    record.is_fraud = final_out.is_fraud;

    if (q.size() >= max_queue_size) {
        q.pop();
        ++dropped_queue_count;
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

    if (final_out.is_fraud) {
        ActiveFraudTrack& active_track = active_fraud_tracks[final_out.object_id];
        active_track.source_object_id = final_out.source_object_id;
        active_track.activated_at = now;
    } else {
        active_fraud_tracks.erase(final_out.object_id);
    }
}

void AnalyticsProcessor::flushReadyAwaitingOutlinesLocked(
    std::chrono::steady_clock::time_point now,
    std::vector<std::string>& outbound_alerts,
    std::vector<OutlineDecisionPayload>& outbound_outline_decisions,
    bool& should_notify_worker) {
    while (!awaiting_outline_queue.empty()) {
        const auto& front = awaiting_outline_queue.front();
        if ((now - front.created_at) < kOutlineRfidGracePeriod) break;

        PendingObject final_out = std::move(awaiting_outline_queue.front());
        awaiting_outline_queue.pop_front();
        finalizePendingObjectLocked(
            final_out, now, outbound_alerts, outbound_outline_decisions, should_notify_worker);
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
            finalized_decisions.erase(it->first);
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

    if (parsed_objects.empty() && parsed_events.empty()) return;

    const auto now = std::chrono::steady_clock::now();
    std::size_t accepted_enter_count = 0;
    bool line_limit_hit = false;
    bool should_notify_worker = false;
    std::vector<std::string> outbound_alerts;
    std::vector<std::string> outbound_enterline_ids;
    std::vector<TrackPosPayload> outbound_track_pos;
    std::vector<OutlineDecisionPayload> outbound_outline_decisions;

    {
        std::lock_guard<std::mutex> lock(mtx);
        pruneExpiredPendingLocked(now);
        pruneExpiredStateLocked(now);
        flushReadyAwaitingOutlinesLocked(
            now, outbound_alerts, outbound_outline_decisions, should_notify_worker);

        const auto state_ttl = std::chrono::seconds(static_cast<long long>(pending_ttl_seconds));
        for (auto it = bbox_aliases_by_event_id.begin(); it != bbox_aliases_by_event_id.end();) {
            if ((now - it->second.updated_at) > state_ttl) {
                it = bbox_aliases_by_event_id.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = active_fraud_tracks.begin(); it != active_fraud_tracks.end();) {
            if ((now - it->second.activated_at) > state_ttl) {
                it = active_fraud_tracks.erase(it);
            } else {
                ++it;
            }
        }

        const auto has_valid_bbox = [](const LatestObjectInfo& info) {
            return info.left >= 0.0f && info.top >= 0.0f &&
                   info.right >= info.left && info.bottom >= info.top;
        };
        const auto is_current_frame_object = [&](const LatestObjectInfo& info) {
            return info.tag_time == tag_time;
        };
        const auto object_center = [](const LatestObjectInfo& info) {
            const double center_x =
                (info.x >= 0.0f) ? static_cast<double>(info.x)
                                 : static_cast<double>(info.left + info.right) * 0.5;
            const double center_y =
                (info.y >= 0.0f) ? static_cast<double>(info.y)
                                 : static_cast<double>(info.top + info.bottom) * 0.5;
            return std::pair<double, double> {center_x, center_y};
        };
        const auto is_source_in_use = [&](const std::string& source_object_id,
                                          const std::string& ignore_event_id) {
            if (source_object_id.empty()) return false;
            for (const auto& entry : active_fraud_tracks) {
                if (entry.first == ignore_event_id) continue;
                if (entry.second.source_object_id == source_object_id) return true;
            }
            return false;
        };
        const auto resolve_source_object_id = [&](const std::string& event_object_id,
                                                  std::string& out_source_object_id) -> bool {
            out_source_object_id.clear();
            if (event_object_id.empty()) return false;

            const LatestObjectInfo* reference_info = nullptr;
            const auto exact_it = latest_objects.find(event_object_id);
            if (exact_it != latest_objects.end() && has_valid_bbox(exact_it->second)) {
                reference_info = &exact_it->second;
                if (is_current_frame_object(exact_it->second)) {
                    out_source_object_id = event_object_id;
                    bbox_aliases_by_event_id[event_object_id] =
                        EventBBoxAlias {out_source_object_id, now};
                    return true;
                }
            }

            const auto alias_it = bbox_aliases_by_event_id.find(event_object_id);
            if (alias_it != bbox_aliases_by_event_id.end()) {
                const auto latest_alias_it = latest_objects.find(alias_it->second.source_object_id);
                if (latest_alias_it != latest_objects.end() && has_valid_bbox(latest_alias_it->second)) {
                    alias_it->second.updated_at = now;
                    if (is_current_frame_object(latest_alias_it->second)) {
                        out_source_object_id = alias_it->second.source_object_id;
                        return true;
                    }
                    if (reference_info == nullptr) {
                        reference_info = &latest_alias_it->second;
                    }
                }
            }

            std::vector<std::pair<std::string, const LatestObjectInfo*>> candidates;
            candidates.reserve(parsed_objects.size());
            for (const auto& parsed_object : parsed_objects) {
                if (parsed_object.id.empty()) continue;
                const auto latest_it = latest_objects.find(parsed_object.id);
                if (latest_it == latest_objects.end()) continue;
                if (!has_valid_bbox(latest_it->second)) continue;
                if (!is_current_frame_object(latest_it->second)) continue;
                candidates.push_back({parsed_object.id, &latest_it->second});
            }
            if (candidates.empty()) return false;

            std::string selected_source_object_id;
            if (reference_info != nullptr) {
                const auto [ref_x, ref_y] = object_center(*reference_info);
                double best_distance_sq = std::numeric_limits<double>::max();
                for (const auto& candidate : candidates) {
                    if (is_source_in_use(candidate.first, event_object_id)) continue;
                    const auto [candidate_x, candidate_y] = object_center(*candidate.second);
                    const double dx = candidate_x - ref_x;
                    const double dy = candidate_y - ref_y;
                    const double distance_sq = (dx * dx) + (dy * dy);
                    if (distance_sq < best_distance_sq) {
                        best_distance_sq = distance_sq;
                        selected_source_object_id = candidate.first;
                    }
                }
                if (selected_source_object_id.empty()) {
                    for (const auto& candidate : candidates) {
                        const auto [candidate_x, candidate_y] = object_center(*candidate.second);
                        const double dx = candidate_x - ref_x;
                        const double dy = candidate_y - ref_y;
                        const double distance_sq = (dx * dx) + (dy * dy);
                        if (distance_sq < best_distance_sq) {
                            best_distance_sq = distance_sq;
                            selected_source_object_id = candidate.first;
                        }
                    }
                }
            } else {
                for (const auto& candidate : candidates) {
                    if (!is_source_in_use(candidate.first, event_object_id)) {
                        selected_source_object_id = candidate.first;
                        break;
                    }
                }
                if (selected_source_object_id.empty()) {
                    selected_source_object_id = candidates.front().first;
                }
            }

            out_source_object_id = selected_source_object_id;
            bbox_aliases_by_event_id[event_object_id] = EventBBoxAlias {out_source_object_id, now};
            return true;
        };
        const auto fill_bbox_from_source = [&](const std::string& source_object_id,
                                               PendingObject& target) -> bool {
            if (source_object_id.empty()) return false;
            const auto latest_it = latest_objects.find(source_object_id);
            if (latest_it == latest_objects.end()) return false;
            if (!has_valid_bbox(latest_it->second)) return false;

            target.source_object_id = source_object_id;
            target.center_x = latest_it->second.x;
            target.center_y = latest_it->second.y;
            target.bbox_left = latest_it->second.left;
            target.bbox_top = latest_it->second.top;
            target.bbox_right = latest_it->second.right;
            target.bbox_bottom = latest_it->second.bottom;
            return true;
        };

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
            std::string event_object_id = trim_copy(event.object_id);
            if (event_object_id.empty()) continue;
            if (event_object_id.size() > kMaxObjectIdBytes) {
                event_object_id.resize(kMaxObjectIdBytes);
            }

            const bool is_outline_rule = (rule_name == outline_rule_name);
            std::string source_object_id;
            std::string tracked_object_id = event_object_id;
            if (resolve_source_object_id(event_object_id, source_object_id) &&
                !source_object_id.empty()) {
                tracked_object_id = source_object_id;
            }

            if (rule_name == enter_rule_name) {
                if (accepted_enter_count >= max_lines_per_batch) {
                    line_limit_hit = true;
                    continue;
                }
                if (pending_object_ids.find(tracked_object_id) != pending_object_ids.end()) continue;
                if (matched_objects.find(tracked_object_id) != matched_objects.end()) continue;

                if (pending_queue.size() >= max_pending_size) {
                    const std::string dropped_id = pending_queue.front().object_id;
                    pending_queue.pop_front();
                    pending_object_ids.erase(dropped_id);
                    ++dropped_pending_overflow_count;
                }

                PendingObject pending;
                pending.object_id = tracked_object_id;
                pending.card_age_text = "0";
                pending.age = generate_weighted_random_age();
                pending.enter_tag_time = event.tag_time;
                pending.is_fraud = true;
                pending.created_at = now;

                if (!source_object_id.empty()) {
                    fill_bbox_from_source(source_object_id, pending);
                }

                pending_queue.push_back(std::move(pending));
                pending_object_ids.insert(tracked_object_id);
                outbound_enterline_ids.push_back(tracked_object_id);
                ++accepted_enter_count;
                continue;
            }

            if (!is_outline_rule) continue;

            PendingObject final_out;
            bool found = false;

            auto matched_it = matched_objects.find(tracked_object_id);
            if (matched_it != matched_objects.end()) {
                final_out = matched_it->second;
                matched_objects.erase(matched_it);
                found = true;
            } else {
                for (auto it = pending_queue.begin(); it != pending_queue.end(); ++it) {
                    if (it->object_id == tracked_object_id) {
                        final_out = *it;
                        pending_queue.erase(it);
                        pending_object_ids.erase(tracked_object_id);
                        found = true;
                        break;
                    }
                }
            }

            if (!found) {
                const auto finalized_it = finalized_decisions.find(tracked_object_id);
                if (finalized_it != finalized_decisions.end()) {
                    continue;
                } else {
                    final_out.object_id = tracked_object_id;
                    final_out.card_age_text = "0";
                    final_out.age = generate_weighted_random_age();
                    final_out.is_fraud = true;
                    final_out.created_at = now;
                }
            }

            final_out.outline_tag_time = event.tag_time;
            if (final_out.source_object_id.empty() && !source_object_id.empty()) {
                final_out.source_object_id = source_object_id;
            }
            if (final_out.source_object_id.empty()) {
                std::string resolved_source_object_id;
                if (resolve_source_object_id(event_object_id, resolved_source_object_id) &&
                    !resolved_source_object_id.empty()) {
                    final_out.source_object_id = resolved_source_object_id;
                }
            }
            if (!final_out.source_object_id.empty()) {
                fill_bbox_from_source(final_out.source_object_id, final_out);
            }

            const bool should_wait_for_late_rfid =
                found && (final_out.card_age_text.empty() || final_out.card_age_text == "0");
            if (should_wait_for_late_rfid) {
                final_out.created_at = now;
                awaiting_outline_queue.push_back(final_out);
                continue;
            }

            finalizePendingObjectLocked(
                final_out, now, outbound_alerts, outbound_outline_decisions, should_notify_worker);

            if (final_out.is_fraud) {
                ActiveFraudTrack& active_track = active_fraud_tracks[final_out.object_id];
                active_track.source_object_id = final_out.source_object_id;
                active_track.activated_at = now;
            } else {
                active_fraud_tracks.erase(final_out.object_id);
            }
        }

        for (auto& entry : active_fraud_tracks) {
            const std::string& fraud_object_id = entry.first;
            ActiveFraudTrack& active_track = entry.second;
            if (active_track.source_object_id.empty()) {
                resolve_source_object_id(fraud_object_id, active_track.source_object_id);
            }
            if (active_track.source_object_id.empty()) continue;

            const auto latest_it = latest_objects.find(active_track.source_object_id);
            if (latest_it == latest_objects.end()) continue;
            if (!has_valid_bbox(latest_it->second)) continue;

            TrackPosPayload track_pos_payload;
            track_pos_payload.object_id = fraud_object_id;
            track_pos_payload.left = latest_it->second.left;
            track_pos_payload.top = latest_it->second.top;
            track_pos_payload.right = latest_it->second.right;
            track_pos_payload.bottom = latest_it->second.bottom;
            track_pos_payload.x = latest_it->second.x;
            track_pos_payload.y = latest_it->second.y;
            outbound_track_pos.push_back(std::move(track_pos_payload));
        }
    }

    if (line_limit_hit) {
        ++dropped_line_limit_count;
    }

    for (const auto& object_id : outbound_enterline_ids) {
        log_enterline_trigger(object_id);
    }
    for (const auto& msg : outbound_alerts) {
        send_alert_to_clients(msg);
    }
    if (outline_decision_callback) {
        for (const auto& payload : outbound_outline_decisions) {
            outline_decision_callback(payload);
        }
    }
    if (track_pos_callback) {
        for (const auto& payload : outbound_track_pos) {
            track_pos_callback(payload);
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
    std::string paired_tag_time;
    bool paired_from_late_outline = false;
    bool should_notify_worker = false;
    std::vector<std::string> outbound_alerts;
    std::vector<OutlineDecisionPayload> outbound_outline_decisions;
    std::vector<std::string> late_outbound_alerts;
    std::vector<OutlineDecisionPayload> late_outbound_outline_decisions;
    bool has_pair_candidate = false;

    {
        std::lock_guard<std::mutex> lock(mtx);
        pruneExpiredPendingLocked(now);
        pruneExpiredStateLocked(now);
        flushReadyAwaitingOutlinesLocked(
            now, outbound_alerts, outbound_outline_decisions, should_notify_worker);

        const bool has_late_outline_waiting = !awaiting_outline_queue.empty();
        has_pair_candidate = has_late_outline_waiting || !pending_queue.empty();
        if (has_pair_candidate) {
            PendingObject pending;
            if (has_late_outline_waiting) {
                pending = std::move(awaiting_outline_queue.front());
                awaiting_outline_queue.pop_front();
                paired_from_late_outline = true;
            } else {
                pending = std::move(pending_queue.front());
                pending_queue.pop_front();
                pending_object_ids.erase(pending.object_id);
            }

            pending.card_age_text = card_age.canonical_text;
            pending.is_fraud = is_fraud_by_age_mismatch(card_age, pending.age);
            paired_object_id = pending.object_id;
            paired_tag_time = pending.enter_tag_time;
            if (paired_from_late_outline && !pending.outline_tag_time.empty()) {
                finalizePendingObjectLocked(
                    pending, now, late_outbound_alerts, late_outbound_outline_decisions,
                    should_notify_worker);
            } else {
                matched_objects[pending.object_id] = std::move(pending);
            }
        }
    }

    for (const auto& msg : outbound_alerts) {
        send_alert_to_clients(msg);
    }
    if (outline_decision_callback) {
        for (const auto& payload : outbound_outline_decisions) {
            outline_decision_callback(payload);
        }
    }
    if (!has_pair_candidate) {
        if (should_notify_worker) {
            cv.notify_one();
        }
        return;
    }

    static std::uint64_t paired_count = 0;
    ++paired_count;
    if (rfid_paired_callback && !paired_object_id.empty()) {
        rfid_paired_callback(paired_object_id, paired_tag_time);
    }
    for (const auto& msg : late_outbound_alerts) {
        send_alert_to_clients(msg);
    }
    if (outline_decision_callback) {
        for (const auto& payload : late_outbound_outline_decisions) {
            outline_decision_callback(payload);
        }
    }
    if (should_notify_worker) {
        cv.notify_one();
    }
}

bool AnalyticsProcessor::insertAnalyticsRow(const FraudRecord& record) {
    if (conn == nullptr || analyticsInsertStmt == nullptr) return false;

    if (mysql_stmt_reset(analyticsInsertStmt) != 0) return false;

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

    if (mysql_stmt_bind_param(analyticsInsertStmt, params) != 0) return false;

    if (mysql_stmt_execute(analyticsInsertStmt) != 0) return false;

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
            insertAnalyticsRow(record);
        }
    }
}
