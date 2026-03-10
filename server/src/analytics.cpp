#include "analytics.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

#include <tinyxml2.h>

#include "alert.h"

namespace {
constexpr const char* kAnalyticsInsertQuery =
    "INSERT INTO analytics_logs (object_id, card_age_text, age_group, is_fraud, created_at) "
    "VALUES (?, ?, ?, ?, NOW())";

std::size_t load_env_size_t(const char* name, std::size_t default_value, std::size_t min_value) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') return default_value;

    errno = 0;
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (errno != 0 || end == raw || (end != nullptr && *end != '\0') || parsed < min_value ||
        parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        std::cerr << "[analytics.cpp] Invalid env " << name << "=" << raw
                  << ", using default=" << default_value << std::endl;
        return default_value;
    }

    return static_cast<std::size_t>(parsed);
}

bool should_sample(std::uint64_t counter, std::size_t interval) {
    if (counter == 1) return true;
    if (interval == 0) return false;
    return (counter % interval) == 0;
}

std::string trim(const std::string& s) {
    std::size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a])) != 0) ++a;
    std::size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])) != 0) --b;
    return s.substr(a, b - a);
}

std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool has_local_name(const char* xml_name, const char* local_name) {
    if (xml_name == nullptr || local_name == nullptr) return false;
    const char* colon = std::strchr(xml_name, ':');
    const char* normalized = (colon != nullptr) ? colon + 1 : xml_name;
    return std::strcmp(normalized, local_name) == 0;
}

bool is_human_type_text(const char* text) {
    if (text == nullptr) return false;
    return to_lower_copy(trim(text)) == "human";
}

bool subtree_has_human_type(tinyxml2::XMLNode* node) {
    for (tinyxml2::XMLNode* cur = node; cur != nullptr; cur = cur->NextSibling()) {
        tinyxml2::XMLElement* element = cur->ToElement();
        if (element != nullptr && has_local_name(element->Name(), "Type")) {
            if (is_human_type_text(element->GetText())) {
                return true;
            }
        }
        if (subtree_has_human_type(cur->FirstChild())) {
            return true;
        }
    }
    return false;
}

void collect_human_object_ids(tinyxml2::XMLNode* node, std::unordered_set<std::string>& out) {
    for (tinyxml2::XMLNode* cur = node; cur != nullptr; cur = cur->NextSibling()) {
        tinyxml2::XMLElement* element = cur->ToElement();
        if (element != nullptr && has_local_name(element->Name(), "Object")) {
            const char* object_id = element->Attribute("ObjectId");
            if (object_id != nullptr && object_id[0] != '\0' &&
                subtree_has_human_type(element->FirstChild())) {
                out.insert(object_id);
            }
        }
        collect_human_object_ids(cur->FirstChild(), out);
    }
}

struct CardAgeDecision {
    const char* canonical_text;
    bool is_fraud;
};

CardAgeDecision evaluate_card_age(std::string_view raw) {
    std::size_t begin = 0;
    while (begin < raw.size() && std::isspace(static_cast<unsigned char>(raw[begin])) != 0) ++begin;
    std::size_t end = raw.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1])) != 0) --end;
    raw = raw.substr(begin, end - begin);

    if (raw == "Adult") return {"Adult", false};
    if (raw == "Senior") return {"Senior", true};
    if (raw == "Youth") return {"Youth", true};
    return {"0", true};
}

std::string fraud_flag(bool is_fraud) {
    return is_fraud ? "Y" : "N";
}
}  // namespace

AnalyticsProcessor::AnalyticsProcessor(const char* h,
                                       const char* u,
                                       const char* p,
                                       const char* d,
                                       int cam_w_,
                                       int cam_h_)
    : host(h ? h : ""),
      user(u ? u : ""),
      pass(p ? p : ""),
      db(d ? d : ""),
      cam_w(cam_w_),
      cam_h(cam_h_),
      conn(nullptr),
      analyticsInsertStmt(nullptr),
      running(false),
      max_lines_per_batch(load_env_size_t("SFEPS_META_MAX_LINES_PER_BATCH", 128, 1)),
      max_queue_size(load_env_size_t("SFEPS_ANALYTICS_QUEUE_MAX", 200, 1)),
      max_pending_size(load_env_size_t("SFEPS_META_PENDING_MAX", 2048, 1)),
      pending_ttl_seconds(load_env_size_t("SFEPS_META_PENDING_TTL_SEC", 30, 1)),
      drop_log_interval(load_env_size_t("SFEPS_DROP_LOG_INTERVAL", 100, 1)),
      dropped_line_limit_count(0),
      dropped_queue_count(0),
      dropped_invalid_xml_count(0),
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
        std::cerr << "[Analytics DB Error] mysql_stmt_init() failed for analytics_logs insert"
                  << std::endl;
        return false;
    }

    if (mysql_stmt_prepare(analyticsInsertStmt, kAnalyticsInsertQuery,
                           std::strlen(kAnalyticsInsertQuery)) != 0) {
        std::cerr << "[Analytics DB Error] prepare failed: "
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
        std::cerr << "[Analytics] mysql_init failed." << std::endl;
        return false;
    }

    if (mysql_real_connect(conn, host.c_str(), user.c_str(), pass.c_str(), db.c_str(), 0, nullptr, 0) ==
        nullptr) {
        std::cerr << "[Analytics] DB connect error: " << mysql_error(conn) << std::endl;
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
    std::cout << "[analytics.cpp] [Analytics] started." << std::endl;
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
        while (!q.empty()) q.pop();
    }

    closeStatements();
    if (conn != nullptr) {
        mysql_close(conn);
        conn = nullptr;
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
        std::cout << "[analytics.cpp] [Drop] expired pending object ids: expired=" << expired
                  << ", total_expired=" << total_expired
                  << ", pending_remaining=" << pending_queue.size() << std::endl;
    }
}

void AnalyticsProcessor::publishRaw(const std::string& raw) {
    if (!running.load()) return;
    if (raw.empty()) return;

    constexpr std::size_t kMaxObjectIdBytes = 128;

    tinyxml2::XMLDocument doc;
    const tinyxml2::XMLError parse_result = doc.Parse(raw.data(), raw.size());
    if (parse_result != tinyxml2::XML_SUCCESS) {
        const std::uint64_t dropped = ++dropped_invalid_xml_count;
        if (should_sample(dropped, drop_log_interval)) {
            std::cout << "[analytics.cpp] [Drop] invalid XML metadata payload: dropped_count="
                      << dropped << ", parse_error=" << parse_result << std::endl;
        }
        return;
    }

    std::unordered_set<std::string> object_ids;
    collect_human_object_ids(doc.FirstChild(), object_ids);

    const std::uint64_t parsed_ok = ++parsed_xml_ok_count;
    if (should_sample(parsed_ok, std::max<std::size_t>(1000, drop_log_interval))) {
        std::cout << "[analytics.cpp] [MetaXML] parsed_ok=" << parsed_ok
                  << ", human_object_candidates=" << object_ids.size() << std::endl;
    }

    if (object_ids.empty()) return;

    std::size_t accepted_count = 0;
    bool line_limit_hit = false;
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(mtx);
        pruneExpiredPendingLocked(now);

        for (const auto& raw_id : object_ids) {
            if (accepted_count >= max_lines_per_batch) {
                line_limit_hit = true;
                break;
            }

            std::string object_id = trim(raw_id);
            if (object_id.empty()) continue;
            if (object_id.size() > kMaxObjectIdBytes) {
                object_id.resize(kMaxObjectIdBytes);
            }

            if (pending_object_ids.find(object_id) != pending_object_ids.end()) {
                continue;
            }

            if (pending_queue.size() >= max_pending_size) {
                const std::string dropped_id = pending_queue.front().object_id;
                pending_queue.pop_front();
                pending_object_ids.erase(dropped_id);

                const std::uint64_t dropped = ++dropped_pending_overflow_count;
                if (should_sample(dropped, drop_log_interval)) {
                    std::cout << "[analytics.cpp] [Drop] pending object queue overflow: max="
                              << max_pending_size << ", dropped_count=" << dropped << std::endl;
                }
            }

            PendingObject pending;
            pending.object_id = object_id;
            pending.card_age_text = "0";
            pending.age_group = "20th";
            pending.is_fraud = false;
            pending.created_at = now;

            pending_queue.push_back(std::move(pending));
            pending_object_ids.insert(object_id);
            ++accepted_count;
        }
    }

    if (line_limit_hit) {
        const std::uint64_t dropped = ++dropped_line_limit_count;
        if (should_sample(dropped, drop_log_interval)) {
            std::cout << "[analytics.cpp] [Drop] human object ids exceeded limit: max="
                      << max_lines_per_batch << ", dropped_count=" << dropped << std::endl;
        }
    }
}

void AnalyticsProcessor::onRfidRead(const std::string& card_age_text_raw) {
    if (!running.load()) return;

    const CardAgeDecision card_age = evaluate_card_age(card_age_text_raw);
    const auto now = std::chrono::steady_clock::now();

    FraudRecord fraud_record;
    bool should_insert = false;

    {
        std::lock_guard<std::mutex> lock(mtx);
        pruneExpiredPendingLocked(now);

        if (pending_queue.empty()) {
            static std::uint64_t no_pending_count = 0;
            ++no_pending_count;
            if (should_sample(no_pending_count, drop_log_interval)) {
                std::cout << "[analytics.cpp] [Matcher] RFID read ignored: no pending object"
                          << std::endl;
            }
            return;
        }

        PendingObject pending = std::move(pending_queue.front());
        pending_queue.pop_front();
        pending_object_ids.erase(pending.object_id);

        pending.card_age_text = card_age.canonical_text;
        pending.is_fraud = card_age.is_fraud;

        if (!pending.is_fraud) {
            static std::uint64_t pass_count = 0;
            ++pass_count;
            if (should_sample(pass_count, std::max<std::size_t>(1000, drop_log_interval))) {
                std::cout << "[analytics.cpp] [Matcher] PASS object_id=" << pending.object_id
                          << ", card_age_text=" << pending.card_age_text
                          << ", age_group=" << pending.age_group << std::endl;
            }
            return;
        }

        fraud_record.object_id = pending.object_id;
        fraud_record.card_age_text = pending.card_age_text;
        fraud_record.age_group = pending.age_group;
        fraud_record.is_fraud = true;

        if (q.size() >= max_queue_size) {
            q.pop();
            const std::uint64_t dropped = ++dropped_queue_count;
            if (should_sample(dropped, drop_log_interval)) {
                std::cout << "[analytics.cpp] [Drop] analytics queue overflow: max="
                          << max_queue_size << ", dropped_count=" << dropped << std::endl;
            }
        }

        q.push(fraud_record);
        should_insert = true;
    }

    if (!should_insert) return;

    const std::string out_message = "FRAUD|" + fraud_record.object_id + "|" +
                                    fraud_record.card_age_text + "|" +
                                    fraud_record.age_group + "|" +
                                    fraud_flag(fraud_record.is_fraud);
    send_alert_to_clients(out_message + "\n");
    cv.notify_one();
}

bool AnalyticsProcessor::insertAnalyticsRow(const FraudRecord& record) {
    if (conn == nullptr || analyticsInsertStmt == nullptr) return false;

    if (mysql_stmt_reset(analyticsInsertStmt) != 0) {
        std::cerr << "[Analytics DB Error] stmt reset failed: "
                  << mysql_stmt_error(analyticsInsertStmt) << std::endl;
        return false;
    }

    MYSQL_BIND params[4];
    std::memset(params, 0, sizeof(params));

    unsigned long object_id_len = static_cast<unsigned long>(record.object_id.size());
    unsigned long card_age_text_len = static_cast<unsigned long>(record.card_age_text.size());
    unsigned long age_group_len = static_cast<unsigned long>(record.age_group.size());
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
    params[2].buffer = const_cast<char*>(record.age_group.c_str());
    params[2].buffer_length = age_group_len;
    params[2].length = &age_group_len;

    params[3].buffer_type = MYSQL_TYPE_TINY;
    params[3].buffer = &fraud_value;
    params[3].is_unsigned = 0;

    if (mysql_stmt_bind_param(analyticsInsertStmt, params) != 0) {
        std::cerr << "[Analytics DB Error] stmt bind failed: "
                  << mysql_stmt_error(analyticsInsertStmt) << std::endl;
        return false;
    }

    if (mysql_stmt_execute(analyticsInsertStmt) != 0) {
        std::cerr << "[Analytics DB Error] stmt execute failed: "
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
                std::cerr << "[Analytics DB Error] failed to insert fraud row: object_id="
                          << record.object_id << ", card_age_text=" << record.card_age_text
                          << std::endl;
            }
        }
    }
}
