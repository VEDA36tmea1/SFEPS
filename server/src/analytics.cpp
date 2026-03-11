#include "analytics.h"
#include "event_matcher.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <tinyxml2.h>

#include "../../Camera/get_metadata/inc/Config.h"

namespace {
constexpr const char* kAnalyticsInsertQuery =
    "INSERT INTO analytics_logs (frame_time, object_type, created_at, estimated_age, photo_path, x, y, event) "
    "VALUES (?, ?, NOW(), ?, ?, ?, ?, ?)";

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

unsigned int parse_time_to_epoch_seconds(const std::string& s) {
    if (s.empty()) {
        return static_cast<unsigned int>(std::time(nullptr));
    }

    std::tm tm {};
    std::istringstream ss(s);
    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (ss.fail()) {
        return static_cast<unsigned int>(std::time(nullptr));
    }

    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    if (t < 0) return static_cast<unsigned int>(std::time(nullptr));
    return static_cast<unsigned int>(t);
}

std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool is_first_or_second_event(const std::string& event_str) {
    if (event_str.empty()) return false;
    const std::string lower = to_lower_copy(event_str);
    return lower.find("first") != std::string::npos || lower.find("second") != std::string::npos;
}

bool has_local_name(const char* xml_name, const char* local_name) {
    if (xml_name == nullptr || local_name == nullptr) return false;
    const char* colon = std::strchr(xml_name, ':');
    const char* normalized = (colon != nullptr) ? colon + 1 : xml_name;
    return std::strcmp(normalized, local_name) == 0;
}

void collect_notification_messages(tinyxml2::XMLNode* node,
                                   std::vector<tinyxml2::XMLElement*>& out) {
    for (tinyxml2::XMLNode* cur = node; cur != nullptr; cur = cur->NextSibling()) {
        tinyxml2::XMLElement* element = cur->ToElement();
        if (element != nullptr && has_local_name(element->Name(), "NotificationMessage")) {
            out.push_back(element);
        }
        collect_notification_messages(cur->FirstChild(), out);
    }
}

bool find_simple_item_value(tinyxml2::XMLNode* node, const char* key, std::string& out) {
    for (tinyxml2::XMLNode* cur = node; cur != nullptr; cur = cur->NextSibling()) {
        tinyxml2::XMLElement* element = cur->ToElement();
        if (element != nullptr && has_local_name(element->Name(), "SimpleItem")) {
            const char* name_attr = element->Attribute("Name");
            if (name_attr != nullptr && std::strcmp(name_attr, key) == 0) {
                const char* value_attr = element->Attribute("Value");
                if (value_attr != nullptr) {
                    out = value_attr;
                    return true;
                }
            }
        }
        if (find_simple_item_value(cur->FirstChild(), key, out)) {
            return true;
        }
    }
    return false;
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
      drop_log_interval(load_env_size_t("SFEPS_DROP_LOG_INTERVAL", 100, 1)),
      dropped_line_limit_count(0),
      dropped_queue_count(0),
      dropped_invalid_xml_count(0) {}

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

    closeStatements();
    if (conn != nullptr) {
        mysql_close(conn);
        conn = nullptr;
    }
}

void AnalyticsProcessor::publishRaw(const std::string& raw) {
    if (!running.load()) return;
    if (raw.empty()) return;

    constexpr std::size_t kMaxRuleNameBytes = 128;
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

    std::size_t line_count = 0;
    bool line_limit_hit = false;
    bool has_event = false;
    std::string extracted_lines;

    std::vector<tinyxml2::XMLElement*> notifications;
    collect_notification_messages(doc.FirstChild(), notifications);
    if (notifications.empty()) {
        return;
    }

    const std::time_t now = std::time(nullptr);
    std::tm* tm = std::localtime(&now);
    char tbuf[80] = {0};
    if (tm != nullptr) {
        std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);
    }
    const std::string now_str = (tm != nullptr) ? std::string(tbuf) : std::string();
    for (tinyxml2::XMLElement* notification : notifications) {
        if (line_count >= max_lines_per_batch) {
            line_limit_hit = true;
            break;
        }
        std::string rule_name;
        std::string state;
        std::string obj_id;
        if (!find_simple_item_value(notification->FirstChild(), "RuleName", rule_name)) {
            continue;
        }
        rule_name = trim(rule_name);
        if (rule_name.empty() || rule_name.size() > kMaxRuleNameBytes) {
            const std::uint64_t dropped = ++dropped_invalid_xml_count;
            if (should_sample(dropped, drop_log_interval)) {
                std::cout << "[analytics.cpp] [Drop] invalid RuleName in XML metadata: dropped_count="
                          << dropped << std::endl;
            }
            continue;
        }

        if (!find_simple_item_value(notification->FirstChild(), "State", state)) {
            continue;
        }
        state = to_lower_copy(trim(state));
        if (!(state == "true" || state == "1")) {
            continue;
        }

        const std::string lower_rule = to_lower_copy(rule_name);
        if (lower_rule.find("first") == std::string::npos &&
            lower_rule.find("second") == std::string::npos) {
            continue;
        }

        if (!find_simple_item_value(notification->FirstChild(), "ObjectId", obj_id)) {
            obj_id = "None";
        }
        obj_id = trim(obj_id);
        if (obj_id.size() > kMaxObjectIdBytes) {
            obj_id.resize(kMaxObjectIdBytes);
        }

        if (has_event) {
            extracted_lines.push_back('\n');
        }
        extracted_lines += "[EVENT] ";
        extracted_lines += rule_name;
        extracted_lines += " Active | ID: ";
        extracted_lines += obj_id;
        extracted_lines += " | Time: ";
        extracted_lines += now_str;

        has_event = true;
        ++line_count;
    }

    if (line_limit_hit) {
        const std::uint64_t dropped = ++dropped_line_limit_count;
        if (should_sample(dropped, drop_log_interval)) {
            std::cout << "[analytics.cpp] [Drop] metadata lines exceeded limit: max="
                      << max_lines_per_batch << ", dropped_count=" << dropped << std::endl;
        }
    }

    if (!has_event) return;

    {
        std::lock_guard<std::mutex> lock(mtx);
        if (q.size() >= max_queue_size) {
            q.pop();
            const std::uint64_t dropped = ++dropped_queue_count;
            if (should_sample(dropped, drop_log_interval)) {
                std::cout << "[analytics.cpp] [Drop] analytics queue overflow: max="
                          << max_queue_size << ", dropped_count=" << dropped << std::endl;
            }
        }
        q.push(std::move(extracted_lines));
    }
    cv.notify_one();
}

bool AnalyticsProcessor::insertAnalyticsRow(const std::string& frame_time,
                                            const std::string& object_type,
                                            int estimated_age,
                                            int x,
                                            int y,
                                            const std::string& event_name,
                                            const std::string& photo_path) {
    if (conn == nullptr || analyticsInsertStmt == nullptr) return false;

    if (mysql_stmt_reset(analyticsInsertStmt) != 0) {
        std::cerr << "[Analytics DB Error] stmt reset failed: "
                  << mysql_stmt_error(analyticsInsertStmt) << std::endl;
        return false;
    }

    MYSQL_BIND params[7];
    std::memset(params, 0, sizeof(params));

    unsigned long frame_time_len = static_cast<unsigned long>(frame_time.size());
    unsigned long object_type_len = static_cast<unsigned long>(object_type.size());
    unsigned long photo_path_len = static_cast<unsigned long>(photo_path.size());
    unsigned long event_name_len = static_cast<unsigned long>(event_name.size());
    int age_param = estimated_age;
    double x_param = static_cast<double>(x);
    double y_param = static_cast<double>(y);

    params[0].buffer_type = MYSQL_TYPE_STRING;
    params[0].buffer = const_cast<char*>(frame_time.c_str());
    params[0].buffer_length = frame_time_len;
    params[0].length = &frame_time_len;

    params[1].buffer_type = MYSQL_TYPE_STRING;
    params[1].buffer = const_cast<char*>(object_type.c_str());
    params[1].buffer_length = object_type_len;
    params[1].length = &object_type_len;

    params[2].buffer_type = MYSQL_TYPE_LONG;
    params[2].buffer = &age_param;

    params[3].buffer_type = MYSQL_TYPE_STRING;
    params[3].buffer = const_cast<char*>(photo_path.c_str());
    params[3].buffer_length = photo_path_len;
    params[3].length = &photo_path_len;

    params[4].buffer_type = MYSQL_TYPE_DOUBLE;
    params[4].buffer = &x_param;

    params[5].buffer_type = MYSQL_TYPE_DOUBLE;
    params[5].buffer = &y_param;

    params[6].buffer_type = MYSQL_TYPE_STRING;
    params[6].buffer = const_cast<char*>(event_name.c_str());
    params[6].buffer_length = event_name_len;
    params[6].length = &event_name_len;

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

void AnalyticsProcessor::processLine(const std::string& rawLine) {
    std::string line = trim(rawLine);
    if (line.empty()) return;

    std::string tag;
    if (line.front() == '[') {
        const size_t close_bracket = line.find(']');
        if (close_bracket != std::string::npos) {
            tag = line.substr(1, close_bracket - 1);
            line = trim(line.substr(close_bracket + 1));
            if (!line.empty() && line.front() == '|') {
                line = trim(line.substr(1));
            }
        }
    }

    std::vector<std::string> parts;
    size_t start = 0;
    while (start < line.size()) {
        const size_t sep = line.find(" | ", start);
        if (sep == std::string::npos) {
            parts.push_back(trim(line.substr(start)));
            break;
        }

        parts.push_back(trim(line.substr(start, sep - start)));
        start = sep + 3;
    }

    std::string id;
    std::string type;
    std::string time_str;
    std::string event_str;
    std::string photo_path;
    int x = 0;
    int y = 0;
    int estimated_age = 0;

    for (const std::string& part : parts) {
        if (part.empty()) continue;

        const size_t colon = part.find(':');
        if (colon == std::string::npos) {
            if (event_str.empty()) {
                event_str = part;
            }
            continue;
        }

        const std::string key = trim(part.substr(0, colon));
        const std::string val = trim(part.substr(colon + 1));

        if (key == "ID") {
            id = val;
        } else if (key == "Type") {
            type = val;
        } else if (key == "Pos") {
            size_t a = val.find('(');
            size_t b = val.find(')');
            std::string coords = val;
            if (a != std::string::npos && b != std::string::npos && b > a) {
                coords = val.substr(a + 1, b - a - 1);
            }

            const size_t comma = coords.find(',');
            if (comma != std::string::npos) {
                const std::string xs = trim(coords.substr(0, comma));
                const std::string ys = trim(coords.substr(comma + 1));
                try {
                    const double nx = std::stod(xs);
                    const double ny = std::stod(ys);
                    x = static_cast<int>(nx * cam_w);
                    y = static_cast<int>(ny * cam_h);
                } catch (...) {
                }
            }
        } else if (key == "Time") {
            time_str = val;
        } else if (key == "Event") {
            event_str = val;
        } else if (key == "Age") {
            try {
                estimated_age = std::stoi(val);
            } catch (...) {
            }
        }
    }

    if (type.empty()) {
        type = "Unknown";
    }
    if (event_str.empty()) {
        event_str = "Detected";
    }

    if (tag != "EVENT") return;
    if (!is_first_or_second_event(event_str)) return;

    if (time_str.empty()) {
        const std::time_t now = std::time(nullptr);
        std::tm* timeinfo = std::localtime(&now);
        char buf[80] = {0};
        if (timeinfo != nullptr) {
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", timeinfo);
            time_str = std::string(buf);
        }
    }

    static std::unordered_map<std::string, unsigned int> gate_last_pass_time;
    const unsigned int event_ts = parse_time_to_epoch_seconds(time_str);

    auto emit_timing_log = [&](const std::string& local_event,
                               const std::string& local_id,
                               unsigned int local_event_ts,
                               unsigned int prev_ts) {
        const double tailgate_sec = static_cast<double>(TAILGATE_LIMIT) / 90000.0;
        if (prev_ts != 0 && local_event_ts >= prev_ts) {
            const double diff_sec = static_cast<double>(local_event_ts - prev_ts);
            if (diff_sec < tailgate_sec) {
                std::cout << "[analytics.cpp] [TAILGATING] " << local_event << " | ID=" << local_id
                          << " | gap=" << diff_sec << "s" << std::endl;
            } else {
                std::cout << "[analytics.cpp] [EVENT] " << local_event << " | ID=" << local_id
                          << " | time=" << time_str << std::endl;
            }
            return;
        }

        std::cout << "[analytics.cpp] [EVENT] " << local_event << " | ID=" << local_id
                  << " | time=" << time_str << std::endl;
    };

    const std::string lower_event = to_lower_copy(event_str);
    const size_t p_first = lower_event.find("first");
    const size_t p_second = lower_event.find("second");

    if (p_first != std::string::npos) {
        std::string gate = event_str.substr(0, p_first);
        gate.erase(std::remove_if(gate.begin(), gate.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
        }),
                   gate.end());

        static std::mt19937 rng(std::random_device {}());
        static const std::vector<std::string> ages = {"Adult", "Senior", "Youth"};
        std::uniform_int_distribution<int> dist(0, static_cast<int>(ages.size()) - 1);
        const std::string assigned = ages[dist(rng)];

        EventMatcher::instance().register_first(gate, id.empty() ? "" : id, assigned, gate, estimated_age);

        unsigned int& prev_ts = gate_last_pass_time[event_str];
        emit_timing_log(event_str, id, event_ts, prev_ts);
        prev_ts = event_ts;
    }

    if (p_second != std::string::npos) {
        std::string gate = event_str.substr(0, p_second);
        gate.erase(std::remove_if(gate.begin(), gate.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
        }),
                   gate.end());

        std::string out_msg;
        EventMatcher::instance().on_second(gate, out_msg);

        unsigned int& prev_second_ts = gate_last_pass_time[event_str];
        emit_timing_log(event_str, id, event_ts, prev_second_ts);
        prev_second_ts = event_ts;
    }

    if (!insertAnalyticsRow(time_str, type, estimated_age, x, y, event_str, photo_path)) {
        std::cerr << "[Analytics DB Error] failed to insert analytics row" << std::endl;
    }
}

void AnalyticsProcessor::workerLoop() {
    while (true) {
        std::vector<std::string> batch;
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

        for (const auto& raw : batch) {
            std::istringstream iss(raw);
            std::string line;
            while (std::getline(iss, line)) {
                processLine(line);
            }
        }
    }
}
