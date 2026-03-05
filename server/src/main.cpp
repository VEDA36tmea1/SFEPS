#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "alert.h"
#include "analytics.h"
#include "audio_common.h"
#include "audio_playback.h"
#include "audio_ring_buffer.h"
#include "auth.h"
#include "cleanup.h"
#include "log.h"
#include "recorder.h"
#include "rfid_monitor.h"
#include "runtime_config.h"
#include "tls_server.h"

namespace fs = std::filesystem;

constexpr int AUTH_PORT = 5555;
constexpr int AUDIO_PORT = 5556;
constexpr int ALERT_PORT = 5557;

std::atomic<bool> g_running(true);

namespace {

struct SecurityRuntimeOptions {
    std::unordered_set<std::string> auth_allow_ips;
    std::unordered_set<std::string> audio_allow_ips;
    std::unordered_set<std::string> alert_allow_ips;
    std::size_t auth_max_bytes = 256;
    std::size_t audio_max_bytes = 4 * 1024 * 1024;
    std::size_t alert_max_clients = 64;
    int socket_read_timeout_ms = 5000;

    bool app_tls_enable = false;
    bool app_plaintext_enable = true;
    int auth_tls_port = 6555;
    int audio_tls_port = 6556;
    int alert_tls_port = 6557;
    std::string app_tls_cert_file;
    std::string app_tls_key_file;
    int app_tls_handshake_timeout_ms = 3000;
};

std::string trim_copy(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
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

bool sleep_interruptible(std::atomic<bool>& running_flag,
                         std::chrono::milliseconds total,
                         std::chrono::milliseconds step = std::chrono::milliseconds(200)) {
    if (step <= std::chrono::milliseconds(0)) {
        step = std::chrono::milliseconds(200);
    }

    std::chrono::milliseconds waited(0);
    while (running_flag.load() && waited < total) {
        const auto remain = total - waited;
        const auto chunk = (remain < step) ? remain : step;
        std::this_thread::sleep_for(chunk);
        waited += chunk;
    }
    return running_flag.load();
}

std::size_t load_env_size_t(const char* name, std::size_t default_value, std::size_t min_value) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') return default_value;

    errno = 0;
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (errno != 0 || end == raw || (end != nullptr && *end != '\0') || parsed < min_value ||
        parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        std::cerr << "[main.cpp] [Config] Invalid env " << name << "=" << raw
                  << ", using default=" << default_value << std::endl;
        return default_value;
    }

    return static_cast<std::size_t>(parsed);
}

int load_env_int(const char* name, int default_value, int min_value) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') return default_value;

    errno = 0;
    char* end = nullptr;
    long parsed = std::strtol(raw, &end, 10);
    if (errno != 0 || end == raw || (end != nullptr && *end != '\0') || parsed < min_value ||
        parsed > std::numeric_limits<int>::max()) {
        std::cerr << "[main.cpp] [Config] Invalid env " << name << "=" << raw
                  << ", using default=" << default_value << std::endl;
        return default_value;
    }

    return static_cast<int>(parsed);
}

bool load_env_bool(const char* name, bool default_value) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') return default_value;

    std::string token(raw);
    std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (token == "1" || token == "true" || token == "yes" || token == "on") return true;
    if (token == "0" || token == "false" || token == "no" || token == "off") return false;

    std::cerr << "[main.cpp] [Config] Invalid boolean env " << name << "=" << raw
              << ", using default=" << (default_value ? "1" : "0") << std::endl;
    return default_value;
}

int load_env_port(const char* name, int default_value) {
    const int parsed = load_env_int(name, default_value, 1);
    if (parsed > 65535) {
        std::cerr << "[main.cpp] [Config] Invalid port env " << name << "=" << parsed
                  << ", using default=" << default_value << std::endl;
        return default_value;
    }
    return parsed;
}

std::unordered_set<std::string> parse_allowlist_env(const char* name) {
    std::unordered_set<std::string> out;
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') return out;

    std::string csv(raw);
    size_t pos = 0;
    while (pos <= csv.size()) {
        size_t comma = csv.find(',', pos);
        std::string token =
            (comma == std::string::npos) ? csv.substr(pos) : csv.substr(pos, comma - pos);
        token = trim_copy(token);
        if (!token.empty()) {
            out.insert(token);
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

SecurityRuntimeOptions load_security_runtime_options() {
    SecurityRuntimeOptions cfg;

    cfg.auth_allow_ips = parse_allowlist_env("SFEPS_AUTH_ALLOW_IPS");
    cfg.audio_allow_ips = parse_allowlist_env("SFEPS_AUDIO_ALLOW_IPS");
    cfg.alert_allow_ips = parse_allowlist_env("SFEPS_ALERT_ALLOW_IPS");
    cfg.auth_max_bytes = load_env_size_t("SFEPS_AUTH_MAX_BYTES", 256, 1);
    cfg.audio_max_bytes = load_env_size_t("SFEPS_AUDIO_MAX_BYTES", 4 * 1024 * 1024, 1024);
    cfg.alert_max_clients = load_env_size_t("SFEPS_ALERT_MAX_CLIENTS", 64, 1);
    cfg.socket_read_timeout_ms = load_env_int("SFEPS_SOCKET_READ_TIMEOUT_MS", 5000, 1);

    cfg.app_tls_enable = load_env_bool("SFEPS_APP_TLS_ENABLE", false);
    cfg.app_plaintext_enable = load_env_bool("SFEPS_APP_PLAINTEXT_ENABLE", true);
    cfg.auth_tls_port = load_env_port("SFEPS_AUTH_TLS_PORT", 6555);
    cfg.audio_tls_port = load_env_port("SFEPS_AUDIO_TLS_PORT", 6556);
    cfg.alert_tls_port = load_env_port("SFEPS_ALERT_TLS_PORT", 6557);
    cfg.app_tls_handshake_timeout_ms =
        load_env_int("SFEPS_APP_TLS_HANDSHAKE_TIMEOUT_MS", 3000, 1);

    const char* cert_file = std::getenv("SFEPS_APP_TLS_CERT_FILE");
    if (cert_file != nullptr && cert_file[0] != '\0') {
        cfg.app_tls_cert_file = cert_file;
    }

    const char* key_file = std::getenv("SFEPS_APP_TLS_KEY_FILE");
    if (key_file != nullptr && key_file[0] != '\0') {
        cfg.app_tls_key_file = key_file;
    }

    return cfg;
}

bool validate_security_runtime_options(const SecurityRuntimeOptions& cfg, std::string& err) {
    if (!cfg.app_plaintext_enable && !cfg.app_tls_enable) {
        err = "invalid runtime config: both plaintext and TLS are disabled";
        return false;
    }

    if (!cfg.app_tls_enable) {
        return true;
    }

    if (cfg.app_tls_cert_file.empty() || cfg.app_tls_key_file.empty()) {
        err = "missing required env for TLS mode: SFEPS_APP_TLS_CERT_FILE/SFEPS_APP_TLS_KEY_FILE";
        return false;
    }

    if (access(cfg.app_tls_cert_file.c_str(), R_OK) != 0) {
        err = "TLS cert file is not readable: " + cfg.app_tls_cert_file;
        return false;
    }

    if (access(cfg.app_tls_key_file.c_str(), R_OK) != 0) {
        err = "TLS key file is not readable: " + cfg.app_tls_key_file;
        return false;
    }

    std::unordered_set<int> tls_ports = {
        cfg.auth_tls_port,
        cfg.audio_tls_port,
        cfg.alert_tls_port,
    };
    if (tls_ports.size() != 3) {
        err = "invalid TLS port config: SFEPS_AUTH/AUDIO/ALERT_TLS_PORT must be unique";
        return false;
    }

    if (cfg.app_plaintext_enable) {
        std::unordered_set<int> plain_ports = {AUTH_PORT, AUDIO_PORT, ALERT_PORT};
        if (plain_ports.count(cfg.auth_tls_port) > 0 || plain_ports.count(cfg.audio_tls_port) > 0 ||
            plain_ports.count(cfg.alert_tls_port) > 0) {
            err = "invalid TLS port config: TLS ports collide with plaintext ports (5555/5556/5557)";
            return false;
        }
    }

    return true;
}

void log_allowlist_mode(const char* env_name, const std::unordered_set<std::string>& allowlist) {
    if (allowlist.empty()) {
        std::cout << "[main.cpp] [Security] " << env_name
                  << " not set: allow-all mode (compatibility)." << std::endl;
        return;
    }

    std::cout << "[main.cpp] [Security] " << env_name << " enabled with " << allowlist.size()
              << " IP(s)." << std::endl;
}

void log_transport_mode(const SecurityRuntimeOptions& cfg) {
    if (cfg.app_plaintext_enable && cfg.app_tls_enable) {
        std::cout << "[main.cpp] [Security] App port mode: dual-stack (plaintext + TLS)."
                  << std::endl;
    } else if (cfg.app_tls_enable) {
        std::cout << "[main.cpp] [Security] App port mode: TLS-only." << std::endl;
    } else {
        std::cout << "[main.cpp] [Security] App port mode: plaintext-only." << std::endl;
    }

    if (cfg.app_tls_enable) {
        std::cout << "[main.cpp] [Security] TLS ports auth/audio/alert=" << cfg.auth_tls_port << "/"
                  << cfg.audio_tls_port << "/" << cfg.alert_tls_port
                  << ", handshake_timeout_ms=" << cfg.app_tls_handshake_timeout_ms << std::endl;
        std::cout << "[main.cpp] [Security] TLS cert file=" << cfg.app_tls_cert_file << std::endl;
    }
}

bool is_ip_allowed(const std::unordered_set<std::string>& allowlist, const std::string& client_ip) {
    if (allowlist.empty()) return true;
    return allowlist.find(client_ip) != allowlist.end();
}

std::string peer_ip_to_string(const sockaddr_in& peer_addr) {
    char ip_buf[INET_ADDRSTRLEN] = {0};
    const char* ip_res =
        inet_ntop(AF_INET, &peer_addr.sin_addr, ip_buf, static_cast<socklen_t>(sizeof(ip_buf)));
    return (ip_res != nullptr) ? std::string(ip_buf) : std::string("Unknown_IP");
}

void apply_socket_read_timeout(int fd, int timeout_ms) {
    if (fd < 0 || timeout_ms <= 0) return;

    timeval tv {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        std::cerr << "[main.cpp] [Security] failed to set SO_RCVTIMEO: " << std::strerror(errno)
                  << std::endl;
    }
}

int create_listen_socket(int port, const char* tag) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "[" << tag << "] socket() failed: " << std::strerror(errno) << std::endl;
        return -1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        std::cerr << "[" << tag << "] setsockopt(SO_REUSEADDR) failed: " << std::strerror(errno)
                  << std::endl;
        close(server_fd);
        return -1;
    }

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[" << tag << "] bind() failed: " << std::strerror(errno) << std::endl;
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 16) != 0) {
        std::cerr << "[" << tag << "] listen() failed: " << std::strerror(errno) << std::endl;
        close(server_fd);
        return -1;
    }

    return server_fd;
}

bool send_all_plain(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        return false;
    }
    return true;
}

bool send_all_tls(const TlsClientConnection& client, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = tls_write(client, data + sent, len - sent);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        return false;
    }
    return true;
}

}  // namespace

void run_audio_receiver(const SecurityRuntimeOptions sec_cfg) {
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
        plain_server_fd = create_listen_socket(AUDIO_PORT, "Audio");
        if (plain_server_fd < 0) {
            ring.stop();
            playback.stop();
            g_running = false;
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
        tls_cfg.tag = "AudioTLS";

        if (!init_tls_server(tls_server, tls_cfg, tls_err)) {
            std::cerr << "[main.cpp] [Audio] failed to start TLS listener: " << tls_err << std::endl;
            if (plain_server_fd >= 0) close(plain_server_fd);
            ring.stop();
            playback.stop();
            g_running = false;
            return;
        }

        std::cout << "[main.cpp] [Audio] listening TLS on port " << sec_cfg.audio_tls_port
                  << std::endl;
    }

    char buf[kBufferSize];
    while (g_running.load()) {
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
                    if (!g_running.load()) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[Audio] accept() failed: " << std::strerror(errno) << std::endl;
                    continue;
                }

                const std::string client_ip = peer_ip_to_string(peer_addr);
                if (!is_ip_allowed(sec_cfg.audio_allow_ips, client_ip)) {
                    std::cout << "[main.cpp] [Audio] Plain connection rejected by allowlist: ip="
                              << client_ip << std::endl;
                    close(client_fd);
                    continue;
                }

                apply_socket_read_timeout(client_fd, sec_cfg.socket_read_timeout_ms);

                bool oversize = false;
                std::size_t total_bytes = 0;
                while (g_running.load()) {
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
                    if (!g_running.load()) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[main.cpp] [Audio] TLS accept failed: " << tls_err << std::endl;
                    continue;
                }

                const std::string client_ip = peer_ip_to_string(peer_addr);
                if (!is_ip_allowed(sec_cfg.audio_allow_ips, client_ip)) {
                    std::cout << "[main.cpp] [Audio] TLS connection rejected by allowlist: ip="
                              << client_ip << std::endl;
                    close_tls_client(client);
                    continue;
                }

                apply_socket_read_timeout(client.fd, sec_cfg.socket_read_timeout_ms);

                bool oversize = false;
                std::size_t total_bytes = 0;
                while (g_running.load()) {
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

void run_fraud_notifier(const SecurityRuntimeOptions sec_cfg) {
    int plain_server_fd = -1;
    if (sec_cfg.app_plaintext_enable) {
        plain_server_fd = create_listen_socket(ALERT_PORT, "Alert");
        if (plain_server_fd < 0) {
            g_running = false;
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
        tls_cfg.tag = "AlertTLS";

        if (!init_tls_server(tls_server, tls_cfg, tls_err)) {
            std::cerr << "[main.cpp] [Alert] failed to start TLS listener: " << tls_err << std::endl;
            if (plain_server_fd >= 0) close(plain_server_fd);
            g_running = false;
            return;
        }

        std::cout << "[main.cpp] [Alert] listening TLS on port " << sec_cfg.alert_tls_port
                  << std::endl;
    }

    while (g_running.load()) {
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
                    if (!g_running.load()) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[Alert] accept() failed: " << std::strerror(errno) << std::endl;
                    continue;
                }

                const std::string client_ip = peer_ip_to_string(peer_addr);
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

                add_alert_plain_client(client_fd);
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
                    if (!g_running.load()) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[main.cpp] [Alert] TLS accept failed: " << tls_err << std::endl;
                    continue;
                }

                const std::string client_ip = peer_ip_to_string(peer_addr);
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

                add_alert_tls_client(std::move(client));
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

void run_login_auth(const RuntimeConfig cfg, const SecurityRuntimeOptions sec_cfg) {
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
        g_running = false;
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
        plain_server_fd = create_listen_socket(AUTH_PORT, "Auth");
        if (plain_server_fd < 0) {
            g_running = false;
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
        tls_cfg.tag = "AuthTLS";

        if (!init_tls_server(tls_server, tls_cfg, tls_err)) {
            std::cerr << "[main.cpp] [Auth] failed to start TLS listener: " << tls_err << std::endl;
            if (plain_server_fd >= 0) close(plain_server_fd);
            g_running = false;
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
            const size_t sep = data.find(':');
            if (sep != std::string::npos) {
                user = trim_copy(data.substr(0, sep));
                const std::string pass = trim_copy(data.substr(sep + 1));
                if (!user.empty() && !pass.empty()) {
                    valid_format = true;
                    const std::string login_key = normalize_login_key(user) + "|" + client_ip;
                    auto now = std::chrono::steady_clock::now();
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
            auto now = std::chrono::steady_clock::now();
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

    while (g_running.load()) {
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
                    if (!g_running.load()) break;
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

                if (oversized) {
                    std::cout
                        << "[main.cpp] [Auth] Plain payload rejected: exceeded SFEPS_AUTH_MAX_BYTES="
                        << sec_cfg.auth_max_bytes << " (ip=" << client_ip << ")" << std::endl;
                }

                const char* resp = success ? "PASS" : "FAIL";
                send_all_plain(client_fd, resp, 4);

                if (success) {
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
                    if (!g_running.load()) break;
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

                if (oversized) {
                    std::cout
                        << "[main.cpp] [Auth] TLS payload rejected: exceeded SFEPS_AUTH_MAX_BYTES="
                        << sec_cfg.auth_max_bytes << " (ip=" << client_ip << ")" << std::endl;
                }

                const char* resp = success ? "PASS" : "FAIL";
                send_all_tls(client, resp, 4);

                if (success) {
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

void signal_handler(int signum) {
    (void)signum;
    g_running = false;
}

int main(int argc, char* argv[]) {
    RuntimeConfig cfg;
    std::string cfg_err;
    if (!load_runtime_config(cfg, cfg_err)) {
        std::cerr << "[Fatal] Runtime config error: " << cfg_err << std::endl;
        return -1;
    }

    const SecurityRuntimeOptions sec_cfg = load_security_runtime_options();
    std::string sec_cfg_err;
    if (!validate_security_runtime_options(sec_cfg, sec_cfg_err)) {
        std::cerr << "[Fatal] Security config error: " << sec_cfg_err << std::endl;
        return -1;
    }

    {
        Authenticator auth_probe(cfg.db_host.c_str(), cfg.db_user.c_str(), cfg.db_pass.c_str(),
                                 cfg.db_name_analytics.c_str());
        if (!auth_probe.connect()) {
            std::cerr << "[Fatal] Auth DB startup check failed (fail-closed)." << std::endl;
            return -1;
        }
    }

    log_allowlist_mode("SFEPS_AUTH_ALLOW_IPS", sec_cfg.auth_allow_ips);
    log_allowlist_mode("SFEPS_AUDIO_ALLOW_IPS", sec_cfg.audio_allow_ips);
    log_allowlist_mode("SFEPS_ALERT_ALLOW_IPS", sec_cfg.alert_allow_ips);
    log_transport_mode(sec_cfg);

    std::cout << "[main.cpp] [Security] auth_max_bytes=" << sec_cfg.auth_max_bytes
              << ", audio_max_bytes=" << sec_cfg.audio_max_bytes
              << ", alert_max_clients=" << sec_cfg.alert_max_clients
              << ", socket_read_timeout_ms=" << sec_cfg.socket_read_timeout_ms << std::endl;

    bool send_test_ping = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--ping-2s" || arg == "--test-ping") {
            send_test_ping = true;
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        if (!fs::exists(VIDEO_SAVE_DIR)) {
            fs::create_directories(VIDEO_SAVE_DIR);
        }
    } catch (const std::exception& e) {
        std::cerr << "[Fatal] Failed to create video directory: " << e.what() << std::endl;
        return -1;
    }

    DBLogger logger(cfg.db_host.c_str(), cfg.db_user.c_str(), cfg.db_pass.c_str(),
                    cfg.db_name_analytics.c_str());
    if (!logger.connect()) {
        std::cerr << "[Fatal] DBLogger startup failed (fail-closed)." << std::endl;
        return -1;
    }

    AnalyticsProcessor analytics(cfg.db_host.c_str(), cfg.db_user.c_str(), cfg.db_pass.c_str(),
                                 cfg.db_name_analytics.c_str(), 3840, 2160);
    if (!analytics.start()) {
        std::cerr << "[Fatal] AnalyticsProcessor startup failed (fail-closed)." << std::endl;
        return -1;
    }

    std::thread t_file_cleanup(run_file_cleanup_worker, std::ref(g_running),
                               std::string(VIDEO_SAVE_DIR), 300);

    std::thread t_db_cleanup([&]() {
        while (g_running.load()) {
            if (!sleep_interruptible(g_running, std::chrono::seconds(60))) break;
            logger.requestDbCleanup();
        }
    });

    std::thread t_auth(run_login_auth, cfg, sec_cfg);
    std::thread t_audio(run_audio_receiver, sec_cfg);
    std::thread t_alert(run_fraud_notifier, sec_cfg);

    RfidMonitor rfid_monitor(g_running);
    std::thread t_rfid(&RfidMonitor::start, &rfid_monitor);

    std::thread t_test_ping;
    if (send_test_ping) {
        t_test_ping = std::thread([&]() {
            int seq = 0;
            while (g_running.load()) {
                send_test_alert_to_clients("TEST|PING|" + std::to_string(seq++));
                if (!sleep_interruptible(g_running, std::chrono::seconds(2))) break;
            }
            std::cout << "[main.cpp] [Alert] test ping thread stopped." << std::endl;
        });
        std::cout << "[main.cpp] [System] Test ping enabled (--test-ping)." << std::endl;
    }

    RTSPRecorder recorder(logger, g_running, analytics);
    recorder.run();

    g_running = false;

    close_alert_client_connections();

    if (t_test_ping.joinable()) t_test_ping.join();
    if (t_rfid.joinable()) t_rfid.join();
    if (t_alert.joinable()) t_alert.join();
    if (t_audio.joinable()) t_audio.join();
    if (t_auth.joinable()) t_auth.join();
    if (t_db_cleanup.joinable()) t_db_cleanup.join();
    if (t_file_cleanup.joinable()) t_file_cleanup.join();

    analytics.stop();

    std::cout << "[main.cpp] [System] server stopped." << std::endl;
    return 0;
}
