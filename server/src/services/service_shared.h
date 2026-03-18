#ifndef APP_SERVICES_SERVICE_SHARED_H
#define APP_SERVICES_SERVICE_SHARED_H

#include <cstddef>
#include <string>

#include <mysql/mysql.h>

#include "analytics.h"

namespace app_services_shared {

constexpr int kAuthPort = 5555;
constexpr int kAudioPort = 5556;
constexpr int kAlertPort = 5557;
constexpr int kPositionPort = 5558;

constexpr std::size_t kMaxObjectIdBytes = 128;
constexpr std::size_t kMaxVideoCatalogRequestBytes = 4096;
constexpr int kDefaultVideoPage = 1;
constexpr int kDefaultVideoPageSize = 20;
constexpr int kMaxVideoPageSize = 100;

struct VideoCatalogRequest {
    std::string from;
    std::string to;
    std::string q;
    int page = kDefaultVideoPage;
    int size = kDefaultVideoPageSize;
};

std::string trim_copy(const std::string& s);
std::string normalize_login_key(const std::string& user);

void mark_ip_authenticated(const std::string& ip);
bool unmark_ip_authenticated(const std::string& ip);
bool is_ip_authenticated(const std::string& ip);

bool snapshots_equal(const AnalyticsProcessor::ObjectPositionSnapshot& lhs,
                     const AnalyticsProcessor::ObjectPositionSnapshot& rhs);
std::string format_obj_pos_line(const AnalyticsProcessor::ObjectPositionSnapshot& snapshot);
std::string format_obj_end_line(const std::string& object_id, const char* reason);
std::string normalize_object_id_token(const std::string& raw);

std::string upper_copy(std::string value);
bool parse_int_in_range(const std::string& raw, int min_value, int max_value, int& out);
std::string mysql_escape_literal(MYSQL* conn, const std::string& input);
std::string normalize_to_iso8601(std::string timestamp);
std::string sanitize_error_field(std::string message);
std::string join_http_url(const std::string& base, const std::string& filename);

bool parse_video_catalog_request(const std::string& line,
                                 VideoCatalogRequest& out,
                                 std::string& out_code,
                                 std::string& out_msg);

}  // namespace app_services_shared

#endif
