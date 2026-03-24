#include "service_shared.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <unordered_set>

#include "text_utils.h"

namespace app_services_shared {

namespace {

struct AuthenticatedIpSessions {
    std::mutex mtx;
    std::unordered_set<std::string> ips;
};

AuthenticatedIpSessions& authenticated_ip_sessions() {
    static AuthenticatedIpSessions sessions;
    return sessions;
}

}  // namespace

std::string trim_copy(const std::string& s) {
    return ::trim_copy(s);
}

std::string normalize_login_key(const std::string& user) {
    std::string normalized = trim_copy(user);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized;
}

void mark_ip_authenticated(const std::string& ip) {
    if (ip.empty()) return;
    auto& sessions = authenticated_ip_sessions();
    std::lock_guard<std::mutex> lock(sessions.mtx);
    sessions.ips.insert(ip);
}

bool unmark_ip_authenticated(const std::string& ip) {
    if (ip.empty()) return false;
    auto& sessions = authenticated_ip_sessions();
    std::lock_guard<std::mutex> lock(sessions.mtx);
    return sessions.ips.erase(ip) > 0;
}

bool is_ip_authenticated(const std::string& ip) {
    if (ip.empty()) return false;
    auto& sessions = authenticated_ip_sessions();
    std::lock_guard<std::mutex> lock(sessions.mtx);
    return sessions.ips.find(ip) != sessions.ips.end();
}

bool snapshots_equal(const AnalyticsProcessor::ObjectPositionSnapshot& lhs,
                     const AnalyticsProcessor::ObjectPositionSnapshot& rhs) {
    return lhs.object_id == rhs.object_id &&
           lhs.left == rhs.left &&
           lhs.top == rhs.top &&
           lhs.right == rhs.right &&
           lhs.bottom == rhs.bottom &&
           lhs.x == rhs.x &&
           lhs.y == rhs.y &&
           lhs.tag_time == rhs.tag_time;
}

std::string format_obj_pos_line(const AnalyticsProcessor::ObjectPositionSnapshot& snapshot) {
    char line[512];
    const int n = std::snprintf(
        line, sizeof(line),
        "OBJ_POS|%s|L=%.1f|T=%.1f|R=%.1f|B=%.1f|X=%.1f|Y=%.1f|FRAUD=%s|TAG=%s\n",
        snapshot.object_id.c_str(), snapshot.left, snapshot.top, snapshot.right, snapshot.bottom,
        snapshot.x, snapshot.y, snapshot.is_fraud ? "Y" : "N", snapshot.tag_time.c_str());
    if (n <= 0 || n >= static_cast<int>(sizeof(line))) return "";
    return std::string(line, static_cast<std::size_t>(n));
}

std::string format_obj_end_line(const std::string& object_id, const char* reason) {
    std::string line =
        "OBJ_END|" + object_id + "|REASON=" + (reason ? std::string(reason) : "UNKNOWN");
    line.push_back('\n');
    return line;
}

std::string normalize_object_id_token(const std::string& raw) {
    std::string object_id = trim_copy(raw);
    if (object_id.size() > kMaxObjectIdBytes) {
        object_id.resize(kMaxObjectIdBytes);
    }
    return object_id;
}

std::string normalize_to_iso8601(std::string timestamp) {
    timestamp = trim_copy(timestamp);
    if (timestamp.size() >= 19 && timestamp[10] == ' ') {
        timestamp[10] = 'T';
        timestamp.resize(19);
    }
    return timestamp;
}

std::string sanitize_error_field(std::string message) {
    for (char& c : message) {
        if (c == '\n' || c == '\r' || c == '|') c = ' ';
    }
    return trim_copy(message);
}

std::string join_http_url(const std::string& base, const std::string& filename) {
    std::string normalized_base = trim_copy(base);
    while (!normalized_base.empty() && normalized_base.back() == '/') {
        normalized_base.pop_back();
    }
    if (normalized_base.empty()) return filename;
    return normalized_base + "/" + filename;
}

}  // namespace app_services_shared
