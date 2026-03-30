#ifndef APP_SERVICES_SERVICE_SHARED_H
#define APP_SERVICES_SERVICE_SHARED_H

#include <cstddef>
#include <string>

namespace app_services_shared {

constexpr int kAuthPort = 5555;
constexpr int kAudioPort = 5556;
constexpr int kAlertPort = 5557;
constexpr int kPositionPort = 5558;

constexpr std::size_t kMaxObjectIdBytes = 128;
constexpr std::size_t kMaxVideoCatalogRequestBytes = 4096;

std::string trim_copy(const std::string& s);
std::string normalize_login_key(const std::string& user);

void mark_ip_authenticated(const std::string& ip);
bool unmark_ip_authenticated(const std::string& ip);
bool is_ip_authenticated(const std::string& ip);

std::string normalize_to_iso8601(std::string timestamp);
std::string sanitize_error_field(std::string message);
std::string join_http_url(const std::string& base, const std::string& filename);

}  // namespace app_services_shared

#endif
