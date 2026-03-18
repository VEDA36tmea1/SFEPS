#include "app_services.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <mysql/mysql.h>

#include "analytics.h"
#include "alert.h"
#include "audio_common.h"
#include "audio_playback.h"
#include "audio_ring_buffer.h"
#include "auth.h"
#include "esp_manager.h"
#include "log.h"
#include "net_utils.h"
#include "tls_server.h"

namespace {

constexpr int AUTH_PORT = 5555;
constexpr int AUDIO_PORT = 5556;
constexpr int ALERT_PORT = 5557;
constexpr int POSITION_PORT = 5558;
constexpr std::size_t kMaxObjectIdBytes = 128;
constexpr std::size_t kMaxVideoCatalogRequestBytes = 4096;
constexpr int kDefaultVideoPage = 1;
constexpr int kDefaultVideoPageSize = 20;
constexpr int kMaxVideoPageSize = 100;

std::string trim_copy(const std::string& s) {
    std::size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    std::size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string normalize_login_key(const std::string& user) {
    std::string normalized = trim_copy(user);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized;
}

struct AuthenticatedIpSessions {
    std::mutex mtx;
    std::unordered_set<std::string> ips;
};

AuthenticatedIpSessions& authenticated_ip_sessions() {
    static AuthenticatedIpSessions sessions;
    return sessions;
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

struct VideoCatalogRequest {
    std::string from;
    std::string to;
    std::string q;
    int page = kDefaultVideoPage;
    int size = kDefaultVideoPageSize;
};

std::string upper_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

bool parse_int_in_range(const std::string& raw, int min_value, int max_value, int& out) {
    if (raw.empty()) return false;

    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(raw.c_str(), &end, 10);
    if (errno != 0 || end == raw.c_str() || (end != nullptr && *end != '\0')) return false;
    if (parsed < min_value || parsed > max_value) return false;

    out = static_cast<int>(parsed);
    return true;
}

std::string mysql_escape_literal(MYSQL* conn, const std::string& input) {
    if (conn == nullptr || input.empty()) return input;

    std::string escaped(input.size() * 2 + 1, '\0');
    const auto escaped_len = mysql_real_escape_string(
        conn, escaped.data(), input.c_str(), static_cast<unsigned long>(input.size()));
    escaped.resize(static_cast<std::size_t>(escaped_len));
    return escaped;
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

bool parse_video_catalog_request(const std::string& line,
                                 VideoCatalogRequest& out,
                                 std::string& out_code,
                                 std::string& out_msg) {
    const std::string trimmed = trim_copy(line);
    if (trimmed.empty()) {
        out_code = "INVALID_REQUEST";
        out_msg = "empty request";
        return false;
    }

    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= trimmed.size()) {
        const std::size_t sep = trimmed.find('|', start);
        if (sep == std::string::npos) {
            parts.push_back(trimmed.substr(start));
            break;
        }
        parts.push_back(trimmed.substr(start, sep - start));
        start = sep + 1;
    }

    if (parts.empty() || trim_copy(parts[0]) != "LIST_REC") {
        out_code = "INVALID_REQUEST";
        out_msg = "expected LIST_REC command";
        return false;
    }

    for (std::size_t i = 1; i < parts.size(); ++i) {
        const std::string token = trim_copy(parts[i]);
        if (token.empty()) continue;

        const std::size_t eq = token.find('=');
        if (eq == std::string::npos) {
            out_code = "INVALID_REQUEST";
            out_msg = "invalid token: " + token;
            return false;
        }

        const std::string key = upper_copy(trim_copy(token.substr(0, eq)));
        const std::string value = trim_copy(token.substr(eq + 1));
        if (key == "FROM") {
            out.from = value;
            continue;
        }
        if (key == "TO") {
            out.to = value;
            continue;
        }
        if (key == "Q") {
            out.q = value;
            continue;
        }
        if (key == "PAGE") {
            if (!parse_int_in_range(value, 1, 1000000, out.page)) {
                out_code = "INVALID_REQUEST";
                out_msg = "PAGE must be integer >= 1";
                return false;
            }
            continue;
        }
        if (key == "SIZE") {
            if (!parse_int_in_range(value, 1, kMaxVideoPageSize, out.size)) {
                out_code = "INVALID_REQUEST";
                out_msg = "SIZE must be integer in range 1..100";
                return false;
            }
            continue;
        }

        out_code = "INVALID_REQUEST";
        out_msg = "unsupported parameter: " + key;
        return false;
    }

    return true;
}

}  // namespace

void run_audio_receiver(std::atomic<bool>& running, const SecurityRuntimeOptions& sec_cfg) {
    constexpr std::size_t kBufferSize = 4096;
    const std::size_t ring_capacity_bytes =
        static_cast<std::size_t>(AUDIO_SAMPLE_RATE * AUDIO_FRAME_BYTES);

    AudioRingBuffer ring(ring_capacity_bytes);
    AudioPlayback playback(ring);
    if (!playback.start()) {
        std::cerr << "[Audio] Failed to start AudioPlayback" << std::endl;
        return;
    }

    int plain_server_fd = -1;
    if (sec_cfg.app_plaintext_enable) {
        plain_server_fd = create_listen_socket(AUDIO_PORT, "Audio", sec_cfg.app_bind_ip);
        if (plain_server_fd < 0) {
            ring.stop();
            playback.stop();
            running = false;
            return;
        }
        std::cout << "[main.cpp] [Audio] listening plaintext on port " << AUDIO_PORT << std::endl;
    }

    TlsServer tls_server;
    if (sec_cfg.app_tls_enable) {
        std::string tls_err;
        TlsServerConfig tls_cfg;
        tls_cfg.port = sec_cfg.audio_tls_port;
        tls_cfg.cert_file = sec_cfg.app_tls_cert_file;
        tls_cfg.key_file = sec_cfg.app_tls_key_file;
        tls_cfg.handshake_timeout_ms = sec_cfg.app_tls_handshake_timeout_ms;
        tls_cfg.bind_ip = sec_cfg.app_bind_ip;
        tls_cfg.tag = "AudioTLS";

        if (!init_tls_server(tls_server, tls_cfg, tls_err)) {
            std::cerr << "[main.cpp] [Audio] failed to start TLS listener: " << tls_err << std::endl;
            if (plain_server_fd >= 0) close(plain_server_fd);
            ring.stop();
            playback.stop();
            running = false;
            return;
        }

        std::cout << "[main.cpp] [Audio] listening TLS on port " << sec_cfg.audio_tls_port
                  << std::endl;
    }

    char buf[kBufferSize];
    while (running.load()) {
        std::vector<pollfd> pfds;
        if (plain_server_fd >= 0) pfds.push_back(pollfd {plain_server_fd, POLLIN, 0});
        if (tls_server.listen_fd >= 0) pfds.push_back(pollfd {tls_server.listen_fd, POLLIN, 0});

        if (pfds.empty()) break;

        const int poll_ret = poll(pfds.data(), pfds.size(), 1000);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Audio] poll() failed: " << std::strerror(errno) << std::endl;
            break;
        }
        if (poll_ret == 0) continue;

        for (const pollfd& pfd : pfds) {
            if ((pfd.revents & POLLIN) == 0) continue;

            if (pfd.fd == plain_server_fd) {
                sockaddr_in peer_addr {};
                socklen_t peer_len = sizeof(peer_addr);
                const int client_fd =
                    accept(plain_server_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
                if (client_fd < 0) {
                    if (!running.load()) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[Audio] accept() failed: " << std::strerror(errno) << std::endl;
                    continue;
                }

                const std::string client_ip = peer_ip_to_string(peer_addr);
                std::cout << "[main.cpp] [Audio] Plain connection attempt: ip=" << client_ip
                          << ", fd=" << client_fd << std::endl;
                if (!is_ip_allowed(sec_cfg.audio_allow_ips, client_ip)) {
                    std::cout << "[main.cpp] [Audio] Plain connection rejected by allowlist: ip="
                              << client_ip << std::endl;
                    close(client_fd);
                    continue;
                }

                apply_socket_read_timeout(client_fd, sec_cfg.socket_read_timeout_ms);

                bool oversize = false;
                std::size_t total_bytes = 0;
                while (running.load()) {
                    const ssize_t bytes_read = read(client_fd, buf, sizeof(buf));
                    if (bytes_read > 0) {
                        const std::size_t chunk = static_cast<std::size_t>(bytes_read);
                        if (total_bytes + chunk > sec_cfg.audio_max_bytes) {
                            oversize = true;
                            break;
                        }
                        ring.push(buf, chunk);
                        total_bytes += chunk;
                        continue;
                    }

                    if (bytes_read == 0) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[Audio] read() failed: " << std::strerror(errno) << std::endl;
                    break;
                }

                if (oversize) {
                    std::cout
                        << "[main.cpp] [Audio] Plain payload rejected: exceeded SFEPS_AUDIO_MAX_BYTES="
                        << sec_cfg.audio_max_bytes << " (ip=" << client_ip << ")" << std::endl;
                }

                close(client_fd);
                continue;
            }

            if (pfd.fd == tls_server.listen_fd) {
                sockaddr_in peer_addr {};
                TlsClientConnection client {};
                std::string tls_err;
                const int client_fd = accept_tls_client(tls_server, client, peer_addr, tls_err);
                if (client_fd < 0) {
                    if (!running.load()) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[main.cpp] [Audio] TLS accept failed: " << tls_err << std::endl;
                    continue;
                }

                const std::string client_ip = peer_ip_to_string(peer_addr);
                std::cout << "[main.cpp] [Audio] TLS connection attempt: ip=" << client_ip
                          << ", fd=" << client_fd << std::endl;
                if (!is_ip_allowed(sec_cfg.audio_allow_ips, client_ip)) {
                    std::cout << "[main.cpp] [Audio] TLS connection rejected by allowlist: ip="
                              << client_ip << std::endl;
                    close_tls_client(client);
                    continue;
                }

                apply_socket_read_timeout(client.fd, sec_cfg.socket_read_timeout_ms);

                bool oversize = false;
                std::size_t total_bytes = 0;
                while (running.load()) {
                    const ssize_t bytes_read = tls_read(client, buf, sizeof(buf));
                    if (bytes_read > 0) {
                        const std::size_t chunk = static_cast<std::size_t>(bytes_read);
                        if (total_bytes + chunk > sec_cfg.audio_max_bytes) {
                            oversize = true;
                            break;
                        }
                        ring.push(buf, chunk);
                        total_bytes += chunk;
                        continue;
                    }

                    if (bytes_read == 0) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[Audio] TLS read() failed: " << std::strerror(errno) << std::endl;
                    break;
                }

                if (oversize) {
                    std::cout
                        << "[main.cpp] [Audio] TLS payload rejected: exceeded SFEPS_AUDIO_MAX_BYTES="
                        << sec_cfg.audio_max_bytes << " (ip=" << client_ip << ")" << std::endl;
                }

                close_tls_client(client);
            }
        }
    }

    if (plain_server_fd >= 0) close(plain_server_fd);
    close_tls_server(tls_server);

    ring.stop();
    playback.stop();
    std::cout << "[main.cpp] [Audio] receiver thread stopped." << std::endl;
}

void run_fraud_notifier(std::atomic<bool>& running, const SecurityRuntimeOptions& sec_cfg) {
    int plain_server_fd = -1;
    if (sec_cfg.app_plaintext_enable) {
        plain_server_fd = create_listen_socket(ALERT_PORT, "Alert", sec_cfg.app_bind_ip);
        if (plain_server_fd < 0) {
            running = false;
            return;
        }
        std::cout << "[main.cpp] [Alert] listening plaintext on port " << ALERT_PORT << std::endl;
    }

    TlsServer tls_server;
    if (sec_cfg.app_tls_enable) {
        std::string tls_err;
        TlsServerConfig tls_cfg;
        tls_cfg.port = sec_cfg.alert_tls_port;
        tls_cfg.cert_file = sec_cfg.app_tls_cert_file;
        tls_cfg.key_file = sec_cfg.app_tls_key_file;
        tls_cfg.handshake_timeout_ms = sec_cfg.app_tls_handshake_timeout_ms;
        tls_cfg.bind_ip = sec_cfg.app_bind_ip;
        tls_cfg.tag = "AlertTLS";

        if (!init_tls_server(tls_server, tls_cfg, tls_err)) {
            std::cerr << "[main.cpp] [Alert] failed to start TLS listener: " << tls_err << std::endl;
            if (plain_server_fd >= 0) close(plain_server_fd);
            running = false;
            return;
        }

        std::cout << "[main.cpp] [Alert] listening TLS on port " << sec_cfg.alert_tls_port
                  << std::endl;
    }

    while (running.load()) {
        std::vector<pollfd> pfds;
        if (plain_server_fd >= 0) pfds.push_back(pollfd {plain_server_fd, POLLIN, 0});
        if (tls_server.listen_fd >= 0) pfds.push_back(pollfd {tls_server.listen_fd, POLLIN, 0});

        if (pfds.empty()) break;

        const int poll_ret = poll(pfds.data(), pfds.size(), 1000);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Alert] poll() failed: " << std::strerror(errno) << std::endl;
            break;
        }
        if (poll_ret == 0) continue;

        for (const pollfd& pfd : pfds) {
            if ((pfd.revents & POLLIN) == 0) continue;

            if (pfd.fd == plain_server_fd) {
                sockaddr_in peer_addr {};
                socklen_t peer_len = sizeof(peer_addr);
                const int client_fd =
                    accept(plain_server_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
                if (client_fd < 0) {
                    if (!running.load()) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[Alert] accept() failed: " << std::strerror(errno) << std::endl;
                    continue;
                }

                const std::string client_ip = peer_ip_to_string(peer_addr);
                std::cout << "[main.cpp] [Alert] Plain connection attempt: ip=" << client_ip
                          << ", fd=" << client_fd << std::endl;
                if (!is_ip_allowed(sec_cfg.alert_allow_ips, client_ip)) {
                    std::cout << "[main.cpp] [Alert] Plain connection rejected by allowlist: ip="
                              << client_ip << std::endl;
                    close(client_fd);
                    continue;
                }

                apply_socket_read_timeout(client_fd, sec_cfg.socket_read_timeout_ms);

                if (alert_client_count() >= sec_cfg.alert_max_clients) {
                    std::cout << "[main.cpp] [Alert] Plain connection rejected: max clients reached ("
                              << sec_cfg.alert_max_clients << ")" << std::endl;
                    close(client_fd);
                    continue;
                }

                add_alert_plain_client(client_fd, client_ip);
                std::cout << "[main.cpp] [Alert] plain client connected: " << client_ip << ":"
                          << ntohs(peer_addr.sin_port) << " (fd=" << client_fd << ")" << std::endl;
                continue;
            }

            if (pfd.fd == tls_server.listen_fd) {
                sockaddr_in peer_addr {};
                TlsClientConnection client {};
                std::string tls_err;
                const int client_fd = accept_tls_client(tls_server, client, peer_addr, tls_err);
                if (client_fd < 0) {
                    if (!running.load()) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[main.cpp] [Alert] TLS accept failed: " << tls_err << std::endl;
                    continue;
                }

                const std::string client_ip = peer_ip_to_string(peer_addr);
                std::cout << "[main.cpp] [Alert] TLS connection attempt: ip=" << client_ip
                          << ", fd=" << client_fd << std::endl;
                if (!is_ip_allowed(sec_cfg.alert_allow_ips, client_ip)) {
                    std::cout << "[main.cpp] [Alert] TLS connection rejected by allowlist: ip="
                              << client_ip << std::endl;
                    close_tls_client(client);
                    continue;
                }

                apply_socket_read_timeout(client.fd, sec_cfg.socket_read_timeout_ms);

                if (alert_client_count() >= sec_cfg.alert_max_clients) {
                    std::cout << "[main.cpp] [Alert] TLS connection rejected: max clients reached ("
                              << sec_cfg.alert_max_clients << ")" << std::endl;
                    close_tls_client(client);
                    continue;
                }

                add_alert_tls_client(std::move(client), client_ip);
                std::cout << "[main.cpp] [Alert] TLS client connected: " << client_ip << ":"
                          << ntohs(peer_addr.sin_port) << " (fd=" << client_fd << ")" << std::endl;
            }
        }
    }

    if (plain_server_fd >= 0) close(plain_server_fd);
    close_tls_server(tls_server);
    close_alert_client_connections();
    std::cout << "[main.cpp] [Alert] notifier thread stopped." << std::endl;
}

void run_video_catalog_service(std::atomic<bool>& running,
                               const RuntimeConfig& cfg,
                               const SecurityRuntimeOptions& sec_cfg) {
    namespace fs = std::filesystem;

    int plain_server_fd = -1;
    if (sec_cfg.app_plaintext_enable) {
        plain_server_fd =
            create_listen_socket(sec_cfg.video_catalog_port, "VideoCatalog", sec_cfg.app_bind_ip);
        if (plain_server_fd < 0) {
            std::cerr << "[main.cpp] [VideoCatalog] plaintext listener disabled." << std::endl;
        } else {
            std::cout << "[main.cpp] [VideoCatalog] listening plaintext on port "
                      << sec_cfg.video_catalog_port << std::endl;
        }
    }

    TlsServer tls_server;
    if (sec_cfg.app_tls_enable) {
        std::string tls_err;
        TlsServerConfig tls_cfg;
        tls_cfg.port = sec_cfg.video_catalog_tls_port;
        tls_cfg.cert_file = sec_cfg.app_tls_cert_file;
        tls_cfg.key_file = sec_cfg.app_tls_key_file;
        tls_cfg.handshake_timeout_ms = sec_cfg.app_tls_handshake_timeout_ms;
        tls_cfg.bind_ip = sec_cfg.app_bind_ip;
        tls_cfg.tag = "VideoCatalogTLS";

        if (!init_tls_server(tls_server, tls_cfg, tls_err)) {
            std::cerr << "[main.cpp] [VideoCatalog] failed to start TLS listener: " << tls_err
                      << std::endl;
        } else {
            std::cout << "[main.cpp] [VideoCatalog] listening TLS on port "
                      << sec_cfg.video_catalog_tls_port << std::endl;
        }
    }

    if (plain_server_fd < 0 && tls_server.listen_fd < 0) {
        std::cerr << "[main.cpp] [VideoCatalog] no listener available. service disabled."
                  << std::endl;
        return;
    }

    MYSQL* db_conn = nullptr;
    auto close_db = [&]() {
        if (db_conn != nullptr) {
            mysql_close(db_conn);
            db_conn = nullptr;
        }
    };
    auto ensure_db = [&]() -> bool {
        if (db_conn != nullptr) return true;

        db_conn = mysql_init(nullptr);
        if (db_conn == nullptr) {
            std::cerr << "[main.cpp] [VideoCatalog] mysql_init failed." << std::endl;
            return false;
        }

        if (mysql_real_connect(db_conn, cfg.db_host.c_str(), cfg.db_user.c_str(),
                               cfg.db_pass.c_str(), cfg.db_name_analytics.c_str(), 3306, nullptr,
                               0) == nullptr) {
            std::cerr << "[main.cpp] [VideoCatalog] DB connect failed: " << mysql_error(db_conn)
                      << std::endl;
            close_db();
            return false;
        }
        return true;
    };

    auto send_error_plain = [&](int fd, const std::string& code, const std::string& msg) {
        const std::string line =
            "REC_ERR|" + sanitize_error_field(code) + "|" + sanitize_error_field(msg) + "\n";
        (void)send_all_plain(fd, line.data(), line.size());
    };
    auto send_error_tls = [&](const TlsClientConnection& conn, const std::string& code,
                              const std::string& msg) {
        const std::string line =
            "REC_ERR|" + sanitize_error_field(code) + "|" + sanitize_error_field(msg) + "\n";
        (void)send_all_tls(conn, line.data(), line.size());
    };

    auto read_request_plain = [&](int fd, std::string& out_request, bool& out_oversized) -> bool {
        out_request.clear();
        out_oversized = false;

        char chunk[1024];
        while (running.load()) {
            const ssize_t n = read(fd, chunk, sizeof(chunk));
            if (n > 0) {
                out_request.append(chunk, static_cast<std::size_t>(n));
                if (out_request.size() > kMaxVideoCatalogRequestBytes) {
                    out_oversized = true;
                    break;
                }
                if (out_request.find('\n') != std::string::npos) break;
                continue;
            }
            if (n == 0) break;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return false;
        }

        if (out_request.empty() && !out_oversized) return false;
        const std::size_t newline = out_request.find('\n');
        if (newline != std::string::npos) out_request.resize(newline);
        out_request = trim_copy(out_request);
        return true;
    };

    auto read_request_tls = [&](const TlsClientConnection& conn, std::string& out_request,
                                bool& out_oversized) -> bool {
        out_request.clear();
        out_oversized = false;

        char chunk[1024];
        while (running.load()) {
            const ssize_t n = tls_read(conn, chunk, sizeof(chunk));
            if (n > 0) {
                out_request.append(chunk, static_cast<std::size_t>(n));
                if (out_request.size() > kMaxVideoCatalogRequestBytes) {
                    out_oversized = true;
                    break;
                }
                if (out_request.find('\n') != std::string::npos) break;
                continue;
            }
            if (n == 0) break;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return false;
        }

        if (out_request.empty() && !out_oversized) return false;
        const std::size_t newline = out_request.find('\n');
        if (newline != std::string::npos) out_request.resize(newline);
        out_request = trim_copy(out_request);
        return true;
    };

    auto handle_request = [&](auto send_line, const std::string& request_line,
                              const std::string& client_ip) -> bool {
        VideoCatalogRequest request;
        std::string parse_code;
        std::string parse_msg;
        if (!parse_video_catalog_request(request_line, request, parse_code, parse_msg)) {
            send_line("REC_ERR|" + sanitize_error_field(parse_code) + "|" +
                      sanitize_error_field(parse_msg) + "\n");
            return false;
        }

        if (!ensure_db()) {
            send_line("REC_ERR|DB_UNAVAILABLE|database connection failed\n");
            return false;
        }

        std::vector<std::string> filters;
        filters.push_back("1=1");
        if (!request.from.empty()) {
            filters.push_back("created_at >= '" + mysql_escape_literal(db_conn, request.from) +
                              "'");
        }
        if (!request.to.empty()) {
            filters.push_back("created_at <= '" + mysql_escape_literal(db_conn, request.to) + "'");
        }
        if (!request.q.empty()) {
            filters.push_back("filename LIKE '%" + mysql_escape_literal(db_conn, request.q) + "%'");
        }

        std::string where_sql;
        for (std::size_t i = 0; i < filters.size(); ++i) {
            if (i > 0) where_sql += " AND ";
            where_sql += filters[i];
        }

        long long total_rows = 0;
        const std::string count_sql =
            "SELECT COUNT(*) FROM recordings WHERE " + where_sql;
        if (mysql_query(db_conn, count_sql.c_str()) != 0) {
            send_line("REC_ERR|DB_ERROR|" + sanitize_error_field(mysql_error(db_conn)) + "\n");
            close_db();
            return false;
        }
        MYSQL_RES* count_res = mysql_store_result(db_conn);
        if (count_res == nullptr) {
            send_line("REC_ERR|DB_ERROR|failed to fetch count result\n");
            close_db();
            return false;
        }
        MYSQL_ROW count_row = mysql_fetch_row(count_res);
        if (count_row != nullptr && count_row[0] != nullptr) {
            total_rows = std::strtoll(count_row[0], nullptr, 10);
            if (total_rows < 0) total_rows = 0;
        }
        mysql_free_result(count_res);

        const long long offset =
            static_cast<long long>(request.page - 1) * static_cast<long long>(request.size);
        const std::string list_sql =
            "SELECT id, filename, DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') "
            "FROM recordings WHERE " +
            where_sql + " ORDER BY created_at DESC LIMIT " + std::to_string(offset) + ", " +
            std::to_string(request.size);

        if (mysql_query(db_conn, list_sql.c_str()) != 0) {
            send_line("REC_ERR|DB_ERROR|" + sanitize_error_field(mysql_error(db_conn)) + "\n");
            close_db();
            return false;
        }

        MYSQL_RES* list_res = mysql_store_result(db_conn);
        if (list_res == nullptr) {
            send_line("REC_ERR|DB_ERROR|failed to fetch recordings result\n");
            close_db();
            return false;
        }

        std::size_t sent_records = 0;
        while (running.load()) {
            MYSQL_ROW row = mysql_fetch_row(list_res);
            if (row == nullptr) break;
            if (row[0] == nullptr || row[1] == nullptr || row[2] == nullptr) continue;

            const std::string id = row[0];
            const std::string filename = row[1];
            const std::string created_at = row[2];

            std::error_code ec;
            if (!fs::exists(filename, ec) || !fs::is_regular_file(filename, ec)) {
                continue;
            }

            const std::string play_url = join_http_url(
                sec_cfg.video_http_base_url, fs::path(filename).filename().string());
            const std::string rec_line =
                "REC|" + id + "|" + normalize_to_iso8601(created_at) + "|0|" + play_url + "\n";
            if (!send_line(rec_line)) {
                mysql_free_result(list_res);
                return false;
            }
            ++sent_records;
        }
        mysql_free_result(list_res);

        const int has_next =
            (offset + static_cast<long long>(request.size) < total_rows) ? 1 : 0;
        const std::string end_line = "REC_END|PAGE=" + std::to_string(request.page) +
                                     "|SIZE=" + std::to_string(request.size) +
                                     "|TOTAL=" + std::to_string(total_rows) +
                                     "|HAS_NEXT=" + std::to_string(has_next) + "\n";
        if (!send_line(end_line)) return false;

        std::cout << "[main.cpp] [VideoCatalog] served request: ip=" << client_ip
                  << ", page=" << request.page << ", size=" << request.size
                  << ", sent=" << sent_records << ", total=" << total_rows << std::endl;
        return true;
    };

    std::size_t active_requests = 0;
    while (running.load()) {
        std::vector<pollfd> pfds;
        if (plain_server_fd >= 0) pfds.push_back(pollfd {plain_server_fd, POLLIN, 0});
        if (tls_server.listen_fd >= 0) pfds.push_back(pollfd {tls_server.listen_fd, POLLIN, 0});
        if (pfds.empty()) break;

        const int poll_ret = poll(pfds.data(), pfds.size(), 1000);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[VideoCatalog] poll() failed: " << std::strerror(errno) << std::endl;
            break;
        }
        if (poll_ret == 0) continue;

        for (const auto& pfd : pfds) {
            if ((pfd.revents & POLLIN) == 0) continue;

            if (pfd.fd == plain_server_fd) {
                sockaddr_in peer_addr {};
                socklen_t peer_len = sizeof(peer_addr);
                const int client_fd =
                    accept(plain_server_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
                if (client_fd < 0) {
                    if (!running.load()) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[VideoCatalog] accept() failed: " << std::strerror(errno)
                              << std::endl;
                    continue;
                }

                const std::string client_ip = peer_ip_to_string(peer_addr);
                if (!is_ip_allowed(sec_cfg.alert_allow_ips, client_ip)) {
                    std::cout << "[main.cpp] [VideoCatalog] Plain connection rejected by allowlist: ip="
                              << client_ip << std::endl;
                    close(client_fd);
                    continue;
                }
                if (active_requests >= sec_cfg.video_max_clients) {
                    send_error_plain(client_fd, "MAX_CLIENTS", "video catalog max clients reached");
                    close(client_fd);
                    continue;
                }
                apply_socket_read_timeout(client_fd, sec_cfg.socket_read_timeout_ms);

                std::string request_line;
                bool oversized = false;
                if (!read_request_plain(client_fd, request_line, oversized)) {
                    close(client_fd);
                    continue;
                }
                if (oversized) {
                    send_error_plain(client_fd, "PAYLOAD_TOO_LARGE",
                                     "request exceeds maximum size");
                    close(client_fd);
                    continue;
                }

                auto send_plain_line = [&](const std::string& line) -> bool {
                    return send_all_plain(client_fd, line.data(), line.size());
                };
                ++active_requests;
                handle_request(send_plain_line, request_line, client_ip);
                if (active_requests > 0) --active_requests;
                close(client_fd);
                continue;
            }

            if (pfd.fd == tls_server.listen_fd) {
                sockaddr_in peer_addr {};
                TlsClientConnection client {};
                std::string tls_err;
                const int client_fd = accept_tls_client(tls_server, client, peer_addr, tls_err);
                if (client_fd < 0) {
                    if (!running.load()) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[main.cpp] [VideoCatalog] TLS accept failed: " << tls_err
                              << std::endl;
                    continue;
                }

                const std::string client_ip = peer_ip_to_string(peer_addr);
                if (!is_ip_allowed(sec_cfg.alert_allow_ips, client_ip)) {
                    std::cout << "[main.cpp] [VideoCatalog] TLS connection rejected by allowlist: ip="
                              << client_ip << std::endl;
                    close_tls_client(client);
                    continue;
                }
                if (active_requests >= sec_cfg.video_max_clients) {
                    send_error_tls(client, "MAX_CLIENTS", "video catalog max clients reached");
                    close_tls_client(client);
                    continue;
                }
                apply_socket_read_timeout(client.fd, sec_cfg.socket_read_timeout_ms);

                std::string request_line;
                bool oversized = false;
                if (!read_request_tls(client, request_line, oversized)) {
                    close_tls_client(client);
                    continue;
                }
                if (oversized) {
                    send_error_tls(client, "PAYLOAD_TOO_LARGE", "request exceeds maximum size");
                    close_tls_client(client);
                    continue;
                }

                auto send_tls_line = [&](const std::string& line) -> bool {
                    return send_all_tls(client, line.data(), line.size());
                };
                ++active_requests;
                handle_request(send_tls_line, request_line, client_ip);
                if (active_requests > 0) --active_requests;
                close_tls_client(client);
            }
        }
    }

    if (plain_server_fd >= 0) close(plain_server_fd);
    close_tls_server(tls_server);
    close_db();
    std::cout << "[main.cpp] [VideoCatalog] service thread stopped." << std::endl;
}

void run_position_stream_service(std::atomic<bool>& running,
                                 const SecurityRuntimeOptions& sec_cfg,
                                 AnalyticsProcessor& analytics,
                                 EspManager& esp_manager) {
    struct PlainClientState {
        int fd = -1;
        std::string client_ip;
        std::string recv_buffer;
        std::string active_object_id;
    };

    struct TlsClientState {
        TlsClientConnection conn {};
        std::string client_ip;
        std::string recv_buffer;
        std::string active_object_id;
    };

    struct PollTarget {
        enum class Kind {
            PlainListen,
            TlsListen,
            PlainClient,
            TlsClient
        };
        Kind kind;
        std::size_t index;
    };

    int plain_server_fd = -1;
    if (sec_cfg.app_plaintext_enable) {
        plain_server_fd = create_listen_socket(POSITION_PORT, "Position", sec_cfg.app_bind_ip);
        if (plain_server_fd < 0) {
            running = false;
            return;
        }
        std::cout << "[main.cpp] [Position] listening plaintext on port " << POSITION_PORT
                  << std::endl;
    }

    TlsServer tls_server;
    if (sec_cfg.app_tls_enable) {
        std::string tls_err;
        TlsServerConfig tls_cfg;
        tls_cfg.port = sec_cfg.position_tls_port;
        tls_cfg.cert_file = sec_cfg.app_tls_cert_file;
        tls_cfg.key_file = sec_cfg.app_tls_key_file;
        tls_cfg.handshake_timeout_ms = sec_cfg.app_tls_handshake_timeout_ms;
        tls_cfg.bind_ip = sec_cfg.app_bind_ip;
        tls_cfg.tag = "PositionTLS";

        if (!init_tls_server(tls_server, tls_cfg, tls_err)) {
            std::cerr << "[main.cpp] [Position] failed to start TLS listener: " << tls_err
                      << std::endl;
            if (plain_server_fd >= 0) close(plain_server_fd);
            running = false;
            return;
        }

        std::cout << "[main.cpp] [Position] listening TLS on port " << sec_cfg.position_tls_port
                  << std::endl;
    }

    auto total_clients = [&](const std::vector<PlainClientState>& plain_clients,
                             const std::vector<TlsClientState>& tls_clients) -> std::size_t {
        return plain_clients.size() + tls_clients.size();
    };

    auto mark_remove = [](std::vector<std::size_t>& remove_indices, std::size_t index) {
        remove_indices.push_back(index);
    };

    auto unique_descending = [](std::vector<std::size_t>& indices) {
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        std::reverse(indices.begin(), indices.end());
    };

    std::vector<PlainClientState> plain_clients;
    std::vector<TlsClientState> tls_clients;
    std::string esp_active_object_id;
    bool esp_has_last_sent = false;
    AnalyticsProcessor::ObjectPositionSnapshot esp_last_sent;
    struct ObjLastSentState {
        std::chrono::steady_clock::time_point updated_at;
        std::chrono::steady_clock::time_point sent_at;
        bool is_fraud = false;
    };
    std::unordered_map<std::string, ObjLastSentState> obj_last_sent;
    std::vector<AnalyticsProcessor::ObjectPositionSnapshot> obj_snapshots;
    std::unordered_set<std::string> obj_ids_this_tick;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> pending_deauth;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        last_force_logout_sent_at;
    const auto deauth_grace =
        std::chrono::milliseconds(std::max(0, sec_cfg.auth_deauth_grace_ms));
    const auto force_logout_cooldown = std::chrono::seconds(5);
    constexpr std::size_t kMaxRecvBuffer = 16 * 1024;
    constexpr std::size_t kReadBufferSize = 4096;
    char read_buffer[kReadBufferSize];
    const auto active_position_connections_for_ip = [&](const std::string& ip) -> std::size_t {
        if (ip.empty()) return 0;

        std::size_t count = 0;
        for (const auto& c : plain_clients) {
            if (c.client_ip == ip) ++count;
        }
        for (const auto& c : tls_clients) {
            if (c.client_ip == ip) ++count;
        }
        return count;
    };
    const auto schedule_deauth = [&](const std::string& ip, const char* reason) {
        if (ip.empty()) return;
        const auto due = std::chrono::steady_clock::now() + deauth_grace;
        pending_deauth[ip] = due;
        std::cout << "[main.cpp] [Auth] deauth scheduled: ip=" << ip
                  << ", grace_ms=" << sec_cfg.auth_deauth_grace_ms
                  << ", reason=" << (reason ? reason : "disconnect") << std::endl;
    };
    const auto cancel_pending_deauth = [&](const std::string& ip) {
        if (ip.empty()) return;
        if (pending_deauth.erase(ip) > 0) {
            std::cout << "[main.cpp] [Auth] deauth canceled (reconnect): ip=" << ip
                      << std::endl;
        }
    };
    const auto run_pending_deauth = [&]() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = pending_deauth.begin(); it != pending_deauth.end();) {
            const std::string ip = it->first;
            if (it->second > now) {
                ++it;
                continue;
            }

            if (active_position_connections_for_ip(ip) > 0) {
                std::cout << "[main.cpp] [Auth] deauth skipped (active position connection): ip="
                          << ip << std::endl;
                it = pending_deauth.erase(it);
                continue;
            }

            const bool removed = unmark_ip_authenticated(ip);
            if (removed) {
                std::cout << "[main.cpp] [Auth] auth session released: ip=" << ip << std::endl;
            }
            it = pending_deauth.erase(it);
        }
    };
    const auto send_force_logout_event = [&](const std::string& ip, const char* proto) {
        if (ip.empty()) return;

        const auto now = std::chrono::steady_clock::now();
        const auto sent_it = last_force_logout_sent_at.find(ip);
        if (sent_it != last_force_logout_sent_at.end() &&
            (now - sent_it->second) < force_logout_cooldown) {
            std::cout << "[main.cpp] [Auth] force logout event skipped (cooldown): ip=" << ip
                      << ", proto=" << (proto ? proto : "UNKNOWN") << std::endl;
            return;
        }

        const std::string logout_msg = "AUTH|FORCE_LOGOUT|REASON=POSITION_UNAUTHENTICATED|PROTO=" +
                                       std::string(proto ? proto : "UNKNOWN") + "\n";
        send_alert_to_ip_clients(ip, logout_msg);
        last_force_logout_sent_at[ip] = now;
        std::cout << "[main.cpp] [Auth] force logout event dispatched: ip=" << ip
                  << ", proto=" << (proto ? proto : "UNKNOWN") << std::endl;
    };
    const auto switch_esp_track_target = [&](const std::string& requested_id) {
        if (requested_id.empty()) return;
        if (!esp_active_object_id.empty() && esp_active_object_id != requested_id) {
            esp_manager.publishTrackEnd(esp_active_object_id, "SWITCH");
        }
        if (esp_active_object_id != requested_id) {
            esp_active_object_id = requested_id;
            esp_has_last_sent = false;
        }
    };
    const auto clear_esp_track_target = [&](const std::string& requested_id, const char* reason) {
        if (requested_id.empty()) return;
        if (esp_active_object_id == requested_id) {
            esp_manager.publishTrackEnd(esp_active_object_id, reason ? reason : "UNSUB");
            esp_active_object_id.clear();
            esp_has_last_sent = false;
        }
    };
    const auto reconcile_esp_track_target = [&]() {
        if (esp_active_object_id.empty()) return;

        bool still_requested = false;
        for (const auto& c : plain_clients) {
            if (c.active_object_id == esp_active_object_id) {
                still_requested = true;
                break;
            }
        }
        if (!still_requested) {
            for (const auto& c : tls_clients) {
                if (c.active_object_id == esp_active_object_id) {
                    still_requested = true;
                    break;
                }
            }
        }
        if (still_requested) return;

        std::string next_target;
        for (const auto& c : plain_clients) {
            if (!c.active_object_id.empty()) {
                next_target = c.active_object_id;
                break;
            }
        }
        if (next_target.empty()) {
            for (const auto& c : tls_clients) {
                if (!c.active_object_id.empty()) {
                    next_target = c.active_object_id;
                    break;
                }
            }
        }

        if (next_target.empty()) {
            std::cout << "[main.cpp] [Position] clearing ESP track target: no active subscribers"
                      << std::endl;
            clear_esp_track_target(esp_active_object_id, "NO_SUBSCRIBER");
            return;
        }

        std::cout << "[main.cpp] [Position] switching ESP track target after disconnect: from="
                  << esp_active_object_id << ", to=" << next_target << std::endl;
        switch_esp_track_target(next_target);
    };

    while (running.load()) {
        std::vector<pollfd> pfds;
        std::vector<PollTarget> targets;

        if (plain_server_fd >= 0) {
            pfds.push_back(pollfd {plain_server_fd, POLLIN, 0});
            targets.push_back(PollTarget {PollTarget::Kind::PlainListen, 0});
        }
        if (tls_server.listen_fd >= 0) {
            pfds.push_back(pollfd {tls_server.listen_fd, POLLIN, 0});
            targets.push_back(PollTarget {PollTarget::Kind::TlsListen, 0});
        }
        for (std::size_t i = 0; i < plain_clients.size(); ++i) {
            pfds.push_back(pollfd {plain_clients[i].fd, POLLIN | POLLHUP | POLLERR, 0});
            targets.push_back(PollTarget {PollTarget::Kind::PlainClient, i});
        }
        for (std::size_t i = 0; i < tls_clients.size(); ++i) {
            pfds.push_back(pollfd {tls_clients[i].conn.fd, POLLIN | POLLHUP | POLLERR, 0});
            targets.push_back(PollTarget {PollTarget::Kind::TlsClient, i});
        }

        const int tick_ms = std::max(1, sec_cfg.position_stream_tick_ms);
        const int poll_ret = poll(pfds.data(), pfds.size(), tick_ms);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Position] poll() failed: " << std::strerror(errno) << std::endl;
            break;
        }

        std::vector<std::size_t> remove_plain;
        std::vector<std::size_t> remove_tls;

        auto send_line_plain = [&](int fd, const std::string& line) -> bool {
            if (line.empty()) return true;
            return send_all_plain(fd, line.data(), line.size());
        };
        auto send_line_tls = [&](const TlsClientConnection& client, const std::string& line) -> bool {
            if (line.empty()) return true;
            return send_all_tls(client, line.data(), line.size());
        };

        if (poll_ret > 0) {
            for (std::size_t i = 0; i < pfds.size(); ++i) {
                const short revents = pfds[i].revents;
                if (revents == 0) continue;

                const PollTarget target = targets[i];

                if (target.kind == PollTarget::Kind::PlainListen) {
                    if ((revents & POLLIN) == 0) continue;
                    sockaddr_in peer_addr {};
                    socklen_t peer_len = sizeof(peer_addr);
                    const int client_fd =
                        accept(plain_server_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
                    if (client_fd < 0) {
                        if (!running.load()) break;
                        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                        std::cerr << "[Position] accept() failed: " << std::strerror(errno)
                                  << std::endl;
                        continue;
                    }

                    const std::string client_ip = peer_ip_to_string(peer_addr);
                    if (!is_ip_allowed(sec_cfg.alert_allow_ips, client_ip)) {
                        std::cout << "[main.cpp] [Position] Plain connection rejected by allowlist: ip="
                                  << client_ip << std::endl;
                        close(client_fd);
                        continue;
                    }
                    if (!is_ip_authenticated(client_ip)) {
                        std::cout
                            << "[main.cpp] [Position] Plain connection rejected: unauthenticated ip="
                            << client_ip << std::endl;
                        send_force_logout_event(client_ip, "PLAIN");
                        close(client_fd);
                        continue;
                    }

                    apply_socket_read_timeout(client_fd, sec_cfg.socket_read_timeout_ms);

                    if (total_clients(plain_clients, tls_clients) >= sec_cfg.position_max_clients) {
                        std::cout << "[main.cpp] [Position] Plain connection rejected: max clients reached ("
                                  << sec_cfg.position_max_clients << ")" << std::endl;
                        close(client_fd);
                        continue;
                    }

                    PlainClientState state;
                    state.fd = client_fd;
                    state.client_ip = client_ip;
                    cancel_pending_deauth(client_ip);
                    plain_clients.push_back(std::move(state));
                    std::cout << "[main.cpp] [Position] plain client connected: " << client_ip << ":"
                              << ntohs(peer_addr.sin_port) << " (fd=" << client_fd << ")"
                              << std::endl;
                    continue;
                }

                if (target.kind == PollTarget::Kind::TlsListen) {
                    if ((revents & POLLIN) == 0) continue;
                    sockaddr_in peer_addr {};
                    TlsClientConnection client {};
                    std::string tls_err;
                    const int client_fd = accept_tls_client(tls_server, client, peer_addr, tls_err);
                    if (client_fd < 0) {
                        if (!running.load()) break;
                        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                        std::cerr << "[main.cpp] [Position] TLS accept failed: " << tls_err
                                  << std::endl;
                        continue;
                    }

                    const std::string client_ip = peer_ip_to_string(peer_addr);
                    if (!is_ip_allowed(sec_cfg.alert_allow_ips, client_ip)) {
                        std::cout << "[main.cpp] [Position] TLS connection rejected by allowlist: ip="
                                  << client_ip << std::endl;
                        close_tls_client(client);
                        continue;
                    }
                    if (!is_ip_authenticated(client_ip)) {
                        std::cout
                            << "[main.cpp] [Position] TLS connection rejected: unauthenticated ip="
                            << client_ip << std::endl;
                        send_force_logout_event(client_ip, "TLS");
                        close_tls_client(client);
                        continue;
                    }

                    apply_socket_read_timeout(client.fd, sec_cfg.socket_read_timeout_ms);

                    if (total_clients(plain_clients, tls_clients) >= sec_cfg.position_max_clients) {
                        std::cout << "[main.cpp] [Position] TLS connection rejected: max clients reached ("
                                  << sec_cfg.position_max_clients << ")" << std::endl;
                        close_tls_client(client);
                        continue;
                    }

                    TlsClientState state;
                    state.conn = std::move(client);
                    state.client_ip = client_ip;
                    cancel_pending_deauth(client_ip);
                    tls_clients.push_back(std::move(state));
                    std::cout << "[main.cpp] [Position] TLS client connected: " << client_ip << ":"
                              << ntohs(peer_addr.sin_port) << " (fd=" << client_fd << ")"
                              << std::endl;
                    continue;
                }

                if (target.kind == PollTarget::Kind::PlainClient) {
                    if (target.index >= plain_clients.size()) continue;
                    auto& client = plain_clients[target.index];

                    if ((revents & (POLLHUP | POLLERR)) != 0) {
                        mark_remove(remove_plain, target.index);
                        continue;
                    }
                    if ((revents & POLLIN) == 0) continue;

                    const ssize_t bytes_read = read(client.fd, read_buffer, sizeof(read_buffer));
                    if (bytes_read == 0) {
                        mark_remove(remove_plain, target.index);
                        continue;
                    }
                    if (bytes_read < 0) {
                        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                        mark_remove(remove_plain, target.index);
                        continue;
                    }

                    client.recv_buffer.append(read_buffer, static_cast<std::size_t>(bytes_read));
                    while (true) {
                        const std::size_t nl = client.recv_buffer.find('\n');
                        if (nl == std::string::npos) break;
                        const std::string line = trim_copy(client.recv_buffer.substr(0, nl));
                        client.recv_buffer.erase(0, nl + 1);
                        if (line.empty()) continue;

                        if (line.rfind("SUB_POS|", 0) == 0) {
                            const std::string requested_id = normalize_object_id_token(line.substr(8));
                            if (requested_id.empty()) continue;

                            std::cout << "[main.cpp] [Position] SUB_POS received: ip="
                                      << client.client_ip << ", object_id=" << requested_id
                                      << std::endl;
                            client.active_object_id = requested_id;
                            switch_esp_track_target(requested_id);
                            continue;
                        }

                        if (line.rfind("UNSUB_POS|", 0) == 0) {
                            const std::string requested_id = normalize_object_id_token(line.substr(10));
                            if (requested_id.empty()) continue;
                            std::cout << "[main.cpp] [Position] UNSUB_POS received: ip="
                                      << client.client_ip << ", object_id=" << requested_id
                                      << std::endl;
                            if (client.active_object_id == requested_id) {
                                client.active_object_id.clear();
                                clear_esp_track_target(requested_id, "UNSUB");
                            }
                            continue;
                        }
                    }

                    if (client.recv_buffer.size() > kMaxRecvBuffer) {
                        client.recv_buffer.clear();
                    }
                    continue;
                }

                if (target.kind == PollTarget::Kind::TlsClient) {
                    if (target.index >= tls_clients.size()) continue;
                    auto& client = tls_clients[target.index];

                    if ((revents & (POLLHUP | POLLERR)) != 0) {
                        mark_remove(remove_tls, target.index);
                        continue;
                    }
                    if ((revents & POLLIN) == 0) continue;

                    const ssize_t bytes_read =
                        tls_read(client.conn, read_buffer, sizeof(read_buffer));
                    if (bytes_read == 0) {
                        mark_remove(remove_tls, target.index);
                        continue;
                    }
                    if (bytes_read < 0) {
                        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                        mark_remove(remove_tls, target.index);
                        continue;
                    }

                    client.recv_buffer.append(read_buffer, static_cast<std::size_t>(bytes_read));
                    while (true) {
                        const std::size_t nl = client.recv_buffer.find('\n');
                        if (nl == std::string::npos) break;
                        const std::string line = trim_copy(client.recv_buffer.substr(0, nl));
                        client.recv_buffer.erase(0, nl + 1);
                        if (line.empty()) continue;

                        if (line.rfind("SUB_POS|", 0) == 0) {
                            const std::string requested_id = normalize_object_id_token(line.substr(8));
                            if (requested_id.empty()) continue;

                            std::cout << "[main.cpp] [Position] SUB_POS received: ip="
                                      << client.client_ip << ", object_id=" << requested_id
                                      << std::endl;
                            client.active_object_id = requested_id;
                            switch_esp_track_target(requested_id);
                            continue;
                        }

                        if (line.rfind("UNSUB_POS|", 0) == 0) {
                            const std::string requested_id = normalize_object_id_token(line.substr(10));
                            if (requested_id.empty()) continue;
                            std::cout << "[main.cpp] [Position] UNSUB_POS received: ip="
                                      << client.client_ip << ", object_id=" << requested_id
                                      << std::endl;
                            if (client.active_object_id == requested_id) {
                                client.active_object_id.clear();
                                clear_esp_track_target(requested_id, "UNSUB");
                            }
                            continue;
                        }
                    }

                    if (client.recv_buffer.size() > kMaxRecvBuffer) {
                        client.recv_buffer.clear();
                    }
                }
            }
        }

        const auto erase_removed_clients = [&]() {
            unique_descending(remove_plain);
            for (const std::size_t idx : remove_plain) {
                if (idx >= plain_clients.size()) continue;
                const std::string disconnected_ip = plain_clients[idx].client_ip;
                if (plain_clients[idx].fd >= 0) close(plain_clients[idx].fd);
                plain_clients.erase(plain_clients.begin() + static_cast<std::ptrdiff_t>(idx));
                schedule_deauth(disconnected_ip, "position_plain_disconnect");
            }
            remove_plain.clear();

            unique_descending(remove_tls);
            for (const std::size_t idx : remove_tls) {
                if (idx >= tls_clients.size()) continue;
                const std::string disconnected_ip = tls_clients[idx].client_ip;
                close_tls_client(tls_clients[idx].conn);
                tls_clients.erase(tls_clients.begin() + static_cast<std::ptrdiff_t>(idx));
                schedule_deauth(disconnected_ip, "position_tls_disconnect");
            }
            remove_tls.clear();
        };

        erase_removed_clients();
        reconcile_esp_track_target();

        const auto now = std::chrono::steady_clock::now();
        const auto esp_stale_limit =
            std::chrono::seconds(static_cast<long long>(sec_cfg.position_stale_seconds));
        const auto obj_stale_limit = std::chrono::seconds(1);
        const auto obj_min_send_interval =
            std::chrono::milliseconds(std::max(1, sec_cfg.position_min_send_ms));

        if (!esp_active_object_id.empty()) {
            AnalyticsProcessor::ObjectPositionSnapshot snapshot;
            const bool has_snapshot =
                analytics.getObjectPositionSnapshot(esp_active_object_id, snapshot);
            const bool is_stale =
                (!has_snapshot) || ((now - snapshot.updated_at) > esp_stale_limit);

            if (is_stale) {
                esp_manager.publishTrackEnd(esp_active_object_id, "STALE");
                esp_active_object_id.clear();
                esp_has_last_sent = false;
            } else if (!esp_has_last_sent || !snapshots_equal(esp_last_sent, snapshot)) {
                EspManager::TrackPosPayload payload;
                payload.object_id = snapshot.object_id;
                payload.left = snapshot.left;
                payload.top = snapshot.top;
                payload.right = snapshot.right;
                payload.bottom = snapshot.bottom;
                payload.x = snapshot.x;
                payload.y = snapshot.y;
                payload.tag_time = snapshot.tag_time;
                if (esp_manager.publishTrackPos(payload)) {
                    esp_last_sent = snapshot;
                    esp_has_last_sent = true;
                }
            }
        }

        const auto broadcast_obj_line = [&](const std::string& line) -> bool {
            if (line.empty()) return true;

            bool delivered = false;
            for (std::size_t idx = 0; idx < plain_clients.size(); ++idx) {
                if (!send_line_plain(plain_clients[idx].fd, line)) {
                    mark_remove(remove_plain, idx);
                    continue;
                }
                delivered = true;
            }
            for (std::size_t idx = 0; idx < tls_clients.size(); ++idx) {
                if (!send_line_tls(tls_clients[idx].conn, line)) {
                    mark_remove(remove_tls, idx);
                    continue;
                }
                delivered = true;
            }
            return delivered;
        };

        analytics.getAllObjectSnapshots(obj_snapshots);
        obj_ids_this_tick.clear();
        obj_ids_this_tick.reserve(obj_snapshots.size());

        for (const auto& snapshot : obj_snapshots) {
            const std::string& object_id = snapshot.object_id;
            if (object_id.empty()) continue;

            obj_ids_this_tick.insert(object_id);
            const bool is_stale = (now - snapshot.updated_at) > obj_stale_limit;

            if (is_stale) {
                const auto sent_it = obj_last_sent.find(object_id);
                if (sent_it == obj_last_sent.end()) continue;

                const std::string end_line = format_obj_end_line(object_id, "STALE");
                broadcast_obj_line(end_line);
                obj_last_sent.erase(object_id);
                continue;
            }

            const auto sent_it = obj_last_sent.find(object_id);
            if (sent_it != obj_last_sent.end() &&
                sent_it->second.updated_at == snapshot.updated_at &&
                sent_it->second.is_fraud == snapshot.is_fraud) {
                continue;
            }
            if (sent_it != obj_last_sent.end() &&
                (now - sent_it->second.sent_at) < obj_min_send_interval) {
                continue;
            }

            const std::string obj_line = format_obj_pos_line(snapshot);
            if (broadcast_obj_line(obj_line)) {
                ObjLastSentState sent_state;
                sent_state.updated_at = snapshot.updated_at;
                sent_state.sent_at = now;
                sent_state.is_fraud = snapshot.is_fraud;
                obj_last_sent[object_id] = sent_state;
            }
        }

        for (auto it = obj_last_sent.begin(); it != obj_last_sent.end();) {
            if (obj_ids_this_tick.find(it->first) == obj_ids_this_tick.end()) {
                const std::string end_line = format_obj_end_line(it->first, "STALE");
                broadcast_obj_line(end_line);
                it = obj_last_sent.erase(it);
            } else {
                ++it;
            }
        }

        erase_removed_clients();
        run_pending_deauth();
    }

    for (auto& client : plain_clients) {
        if (client.fd >= 0) close(client.fd);
    }
    for (auto& client : tls_clients) {
        close_tls_client(client.conn);
    }
    if (plain_server_fd >= 0) close(plain_server_fd);
    close_tls_server(tls_server);
    std::cout << "[main.cpp] [Position] stream service thread stopped." << std::endl;
}

void run_login_auth(std::atomic<bool>& running,
                    const RuntimeConfig& cfg,
                    const SecurityRuntimeOptions& sec_cfg) {
    struct AttemptState {
        int fail_count = 0;
        std::chrono::steady_clock::time_point lock_until =
            std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point last_seen =
            std::chrono::steady_clock::time_point::min();
    };

    constexpr int kMaxFail = 5;
    constexpr int kCleanupInterval = 100;
    const auto kLockDuration = std::chrono::seconds(30);
    const auto kStaleRetention = std::chrono::minutes(10);

    Authenticator auth(cfg.db_host.c_str(), cfg.db_user.c_str(), cfg.db_pass.c_str(),
                       cfg.db_name_analytics.c_str());
    if (!auth.connect()) {
        std::cerr << "[main.cpp] [Fatal] Auth DB connection failed (fail-closed)." << std::endl;
        running = false;
        return;
    }

    DBLogger auth_logger(cfg.db_host.c_str(), cfg.db_user.c_str(), cfg.db_pass.c_str(),
                         cfg.db_name_analytics.c_str());
    if (!auth_logger.connect()) {
        std::cerr << "[main.cpp] [Warn] Auth logger DB connection failed. "
                  << "Login service will continue without auth DB log writes." << std::endl;
    }

    std::cout << "[AuthFlow][4] run_login_auth started. "
              << "plaintext=" << (sec_cfg.app_plaintext_enable ? "on" : "off")
              << ", tls=" << (sec_cfg.app_tls_enable ? "on" : "off")
              << ", auth_tls_port=" << sec_cfg.auth_tls_port << std::endl;

    int plain_server_fd = -1;
    if (sec_cfg.app_plaintext_enable) {
        plain_server_fd = create_listen_socket(AUTH_PORT, "Auth", sec_cfg.app_bind_ip);
        if (plain_server_fd < 0) {
            running = false;
            return;
        }
        std::cout << "[main.cpp] [Auth] listening plaintext on port " << AUTH_PORT << std::endl;
        std::cout << "[AuthFlow][4] auth plaintext listener ready on " << AUTH_PORT << std::endl;
    }

    TlsServer tls_server;
    if (sec_cfg.app_tls_enable) {
        std::string tls_err;
        TlsServerConfig tls_cfg;
        tls_cfg.port = sec_cfg.auth_tls_port;
        tls_cfg.cert_file = sec_cfg.app_tls_cert_file;
        tls_cfg.key_file = sec_cfg.app_tls_key_file;
        tls_cfg.handshake_timeout_ms = sec_cfg.app_tls_handshake_timeout_ms;
        tls_cfg.bind_ip = sec_cfg.app_bind_ip;
        tls_cfg.tag = "AuthTLS";

        if (!init_tls_server(tls_server, tls_cfg, tls_err)) {
            std::cerr << "[main.cpp] [Auth] failed to start TLS listener: " << tls_err << std::endl;
            if (plain_server_fd >= 0) close(plain_server_fd);
            running = false;
            return;
        }

        std::cout << "[main.cpp] [Auth] listening TLS on port " << sec_cfg.auth_tls_port
                  << std::endl;
        std::cout << "[AuthFlow][4] auth TLS listener ready on " << sec_cfg.auth_tls_port
                  << std::endl;
    }

    std::unordered_map<std::string, AttemptState> attempts;
    int request_counter = 0;

    auto evaluate_auth = [&](const std::string& data,
                             const std::string& client_ip,
                             bool oversized,
                             std::string& user,
                             bool& success) {
        user = "Unknown";
        success = false;
        bool valid_format = false;

        if (!oversized) {
            const std::size_t sep = data.find(':');
            if (sep != std::string::npos) {
                user = trim_copy(data.substr(0, sep));
                const std::string pass = trim_copy(data.substr(sep + 1));
                if (!user.empty() && !pass.empty()) {
                    valid_format = true;
                    const std::string login_key = normalize_login_key(user) + "|" + client_ip;
                    const auto now = std::chrono::steady_clock::now();
                    AttemptState& state = attempts[login_key];
                    state.last_seen = now;

                    if (state.lock_until > now) {
                        success = false;
                    } else {
                        success = auth.authenticate(user, pass);
                        if (success) {
                            state.fail_count = 0;
                            state.lock_until = std::chrono::steady_clock::time_point::min();
                        } else {
                            state.fail_count += 1;
                            if (state.fail_count >= kMaxFail) {
                                state.fail_count = 0;
                                state.lock_until = now + kLockDuration;
                            }
                        }
                    }
                }
            }
        }

        if (oversized) {
            success = false;
        } else if (!valid_format) {
            success = false;
        }

        if (++request_counter % kCleanupInterval == 0) {
            const auto now = std::chrono::steady_clock::now();
            for (auto it = attempts.begin(); it != attempts.end();) {
                const bool is_locked = it->second.lock_until > now;
                const bool is_stale =
                    it->second.last_seen != std::chrono::steady_clock::time_point::min() &&
                    ((now - it->second.last_seen) > kStaleRetention);
                if (!is_locked && it->second.fail_count == 0 && is_stale) {
                    it = attempts.erase(it);
                } else {
                    ++it;
                }
            }
        }
    };

    while (running.load()) {
        std::vector<pollfd> pfds;
        if (plain_server_fd >= 0) pfds.push_back(pollfd {plain_server_fd, POLLIN, 0});
        if (tls_server.listen_fd >= 0) pfds.push_back(pollfd {tls_server.listen_fd, POLLIN, 0});

        if (pfds.empty()) break;

        const int poll_ret = poll(pfds.data(), pfds.size(), 1000);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Auth] poll() failed: " << std::strerror(errno) << std::endl;
            break;
        }
        if (poll_ret == 0) continue;

        for (const pollfd& pfd : pfds) {
            if ((pfd.revents & POLLIN) == 0) continue;

            if (pfd.fd == plain_server_fd) {
                sockaddr_in peer_addr {};
                socklen_t peer_len = sizeof(peer_addr);
                const int client_fd =
                    accept(plain_server_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
                if (client_fd < 0) {
                    if (!running.load()) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[Auth] accept() failed: " << std::strerror(errno) << std::endl;
                    continue;
                }

                const std::string client_ip = peer_ip_to_string(peer_addr);
                std::cout << "[AuthFlow][4] plain auth client accepted: ip=" << client_ip
                          << ", fd=" << client_fd << std::endl;
                if (!is_ip_allowed(sec_cfg.auth_allow_ips, client_ip)) {
                    std::cout << "[main.cpp] [Auth] Plain connection rejected by allowlist: ip="
                              << client_ip << std::endl;
                    close(client_fd);
                    continue;
                }

                apply_socket_read_timeout(client_fd, sec_cfg.socket_read_timeout_ms);

                std::vector<char> buf(sec_cfg.auth_max_bytes + 1, 0);
                const ssize_t bytes_read = read(client_fd, buf.data(), buf.size());
                if (bytes_read <= 0) {
                    if (bytes_read < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                        std::cerr << "[Auth] read() failed: " << std::strerror(errno) << std::endl;
                    }
                    close(client_fd);
                    continue;
                }

                const bool oversized = static_cast<std::size_t>(bytes_read) > sec_cfg.auth_max_bytes;
                const std::size_t data_len =
                    oversized ? sec_cfg.auth_max_bytes : static_cast<std::size_t>(bytes_read);
                const std::string data(buf.data(), data_len);

                std::string user;
                bool success = false;
                evaluate_auth(data, client_ip, oversized, user, success);
                std::cout << "[main.cpp] [Auth] Plain login attempt result: ip=" << client_ip
                          << ", user=" << user << ", result=" << (success ? "PASS" : "FAIL")
                          << std::endl;

                if (oversized) {
                    std::cout
                        << "[main.cpp] [Auth] Plain payload rejected: exceeded SFEPS_AUTH_MAX_BYTES="
                        << sec_cfg.auth_max_bytes << " (ip=" << client_ip << ")" << std::endl;
                }

                const char* resp = success ? "PASS" : "FAIL";
                send_all_plain(client_fd, resp, 4);

                if (success) {
                    mark_ip_authenticated(client_ip);
                    std::cout << "[main.cpp] [Auth] Plain login session registered: ip="
                              << client_ip << ", user=" << user << std::endl;
                    send_alert_to_clients("TEST|LOGIN_OK|" + user + "\n");
                }
                auth_logger.enqueueLogin(user, client_ip, success);

                close(client_fd);
                continue;
            }

            if (pfd.fd == tls_server.listen_fd) {
                sockaddr_in peer_addr {};
                TlsClientConnection client {};
                std::string tls_err;
                const int client_fd = accept_tls_client(tls_server, client, peer_addr, tls_err);
                if (client_fd < 0) {
                    if (!running.load()) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[main.cpp] [Auth] TLS accept failed: " << tls_err << std::endl;
                    continue;
                }

                const std::string client_ip = peer_ip_to_string(peer_addr);
                std::cout << "[AuthFlow][4] TLS auth client accepted: ip=" << client_ip
                          << ", fd=" << client_fd << std::endl;
                if (!is_ip_allowed(sec_cfg.auth_allow_ips, client_ip)) {
                    std::cout << "[main.cpp] [Auth] TLS connection rejected by allowlist: ip="
                              << client_ip << std::endl;
                    close_tls_client(client);
                    continue;
                }

                apply_socket_read_timeout(client.fd, sec_cfg.socket_read_timeout_ms);

                std::vector<char> buf(sec_cfg.auth_max_bytes + 1, 0);
                const ssize_t bytes_read = tls_read(client, buf.data(), buf.size());
                if (bytes_read <= 0) {
                    if (bytes_read < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                        std::cerr << "[Auth] TLS read() failed: " << std::strerror(errno) << std::endl;
                    }
                    close_tls_client(client);
                    continue;
                }

                const bool oversized = static_cast<std::size_t>(bytes_read) > sec_cfg.auth_max_bytes;
                const std::size_t data_len =
                    oversized ? sec_cfg.auth_max_bytes : static_cast<std::size_t>(bytes_read);
                const std::string data(buf.data(), data_len);

                std::string user;
                bool success = false;
                evaluate_auth(data, client_ip, oversized, user, success);
                std::cout << "[main.cpp] [Auth] TLS login attempt result: ip=" << client_ip
                          << ", user=" << user << ", result=" << (success ? "PASS" : "FAIL")
                          << std::endl;

                if (oversized) {
                    std::cout
                        << "[main.cpp] [Auth] TLS payload rejected: exceeded SFEPS_AUTH_MAX_BYTES="
                        << sec_cfg.auth_max_bytes << " (ip=" << client_ip << ")" << std::endl;
                }

                const char* resp = success ? "PASS" : "FAIL";
                send_all_tls(client, resp, 4);

                if (success) {
                    mark_ip_authenticated(client_ip);
                    std::cout << "[main.cpp] [Auth] TLS login session registered: ip="
                              << client_ip << ", user=" << user << std::endl;
                    send_alert_to_clients("TEST|LOGIN_OK|" + user + "\n");
                }
                auth_logger.enqueueLogin(user, client_ip, success);

                close_tls_client(client);
            }
        }
    }

    if (plain_server_fd >= 0) close(plain_server_fd);
    close_tls_server(tls_server);

    std::cout << "[main.cpp] [Auth] auth thread stopped." << std::endl;
}
