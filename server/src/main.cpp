#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <unistd.h>

#include "app_services.h"
#include "alert.h"
#include "analytics.h"
#include "auth.h"
#include "cleanup.h"
#include "esp_manager.h"
#include "log.h"
#include "recorder.h"
#include "rfid_monitor.h"
#include "runtime_config.h"

namespace fs = std::filesystem;

constexpr int AUTH_PORT = 5555;
constexpr int AUDIO_PORT = 5556;
constexpr int ALERT_PORT = 5557;
constexpr int POSITION_PORT = 5558;

std::atomic<bool> g_running(true);

namespace {

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
    cfg.position_max_clients = load_env_size_t("SFEPS_POSITION_MAX_CLIENTS", 64, 1);
    cfg.socket_read_timeout_ms = load_env_int("SFEPS_SOCKET_READ_TIMEOUT_MS", 5000, 1);
    cfg.position_stream_tick_ms = load_env_int("SFEPS_POSITION_TICK_MS", 100, 1);
    cfg.position_stale_seconds = load_env_size_t("SFEPS_POSITION_STALE_SEC", 3, 1);

    cfg.app_tls_enable = load_env_bool("SFEPS_APP_TLS_ENABLE", false);
    cfg.app_plaintext_enable = load_env_bool("SFEPS_APP_PLAINTEXT_ENABLE", true);
    cfg.auth_tls_port = load_env_port("SFEPS_AUTH_TLS_PORT", 6555);
    cfg.audio_tls_port = load_env_port("SFEPS_AUDIO_TLS_PORT", 6556);
    cfg.alert_tls_port = load_env_port("SFEPS_ALERT_TLS_PORT", 6557);
    cfg.position_tls_port = load_env_port("SFEPS_POSITION_TLS_PORT", 6558);
    cfg.app_tls_handshake_timeout_ms =
        load_env_int("SFEPS_APP_TLS_HANDSHAKE_TIMEOUT_MS", 3000, 1);
    const char* bind_ip = std::getenv("SFEPS_APP_BIND_IP");
    if (bind_ip != nullptr && bind_ip[0] != '\0') {
        cfg.app_bind_ip = trim_copy(bind_ip);
    }

    const char* cert_file = std::getenv("SFEPS_APP_TLS_CERT_FILE");
    if (cert_file != nullptr && cert_file[0] != '\0') {
        cfg.app_tls_cert_file = cert_file;
    }

    const char* key_file = std::getenv("SFEPS_APP_TLS_KEY_FILE");
    if (key_file != nullptr && key_file[0] != '\0') {
        cfg.app_tls_key_file = key_file;
    }

    cfg.esp_tcp_enable = load_env_bool("SFEPS_ESP_TCP_ENABLE", false);
    cfg.esp_tcp_port = load_env_port("SFEPS_ESP_TCP_PORT", 5565);
    cfg.esp_tcp_max_clients = load_env_size_t("SFEPS_ESP_TCP_MAX_CLIENTS", 4, 1);
    cfg.esp_tcp_allow_ips = parse_allowlist_env("SFEPS_ESP_TCP_ALLOW_IPS");
    const char* esp_bind_ip = std::getenv("SFEPS_ESP_TCP_BIND_IP");
    if (esp_bind_ip != nullptr && esp_bind_ip[0] != '\0') {
        cfg.esp_tcp_bind_ip = trim_copy(esp_bind_ip);
    }

    return cfg;
}

bool validate_security_runtime_options(const SecurityRuntimeOptions& cfg, std::string& err) {
    if (!cfg.app_plaintext_enable && !cfg.app_tls_enable) {
        err = "invalid runtime config: both plaintext and TLS are disabled";
        return false;
    }

    in_addr bind_addr {};
    if (inet_pton(AF_INET, cfg.app_bind_ip.c_str(), &bind_addr) != 1) {
        err = "invalid bind IP in SFEPS_APP_BIND_IP: " + cfg.app_bind_ip;
        return false;
    }

    if (cfg.esp_tcp_enable) {
        in_addr esp_bind_addr {};
        if (inet_pton(AF_INET, cfg.esp_tcp_bind_ip.c_str(), &esp_bind_addr) != 1) {
            err = "invalid bind IP in SFEPS_ESP_TCP_BIND_IP: " + cfg.esp_tcp_bind_ip;
            return false;
        }
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
        cfg.position_tls_port,
    };
    if (tls_ports.size() != 4) {
        err = "invalid TLS port config: SFEPS_AUTH/AUDIO/ALERT/POSITION_TLS_PORT must be unique";
        return false;
    }

    if (cfg.app_plaintext_enable) {
        std::unordered_set<int> plain_ports = {AUTH_PORT, AUDIO_PORT, ALERT_PORT, POSITION_PORT};
        if (plain_ports.count(cfg.auth_tls_port) > 0 || plain_ports.count(cfg.audio_tls_port) > 0 ||
            plain_ports.count(cfg.alert_tls_port) > 0 ||
            plain_ports.count(cfg.position_tls_port) > 0) {
            err = "invalid TLS port config: TLS ports collide with plaintext ports (5555/5556/5557/5558)";
            return false;
        }
    }

    if (cfg.auth_allow_ips.empty()) {
        err = "missing required allowlist: SFEPS_AUTH_ALLOW_IPS (fail-closed)";
        return false;
    }
    if (cfg.audio_allow_ips.empty()) {
        err = "missing required allowlist: SFEPS_AUDIO_ALLOW_IPS (fail-closed)";
        return false;
    }
    if (cfg.alert_allow_ips.empty()) {
        err = "missing required allowlist: SFEPS_ALERT_ALLOW_IPS (fail-closed)";
        return false;
    }

    return true;
}

void log_allowlist_mode(const char* env_name, const std::unordered_set<std::string>& allowlist) {
    if (allowlist.empty()) {
        std::cout << "[main.cpp] [Security] " << env_name
                  << " is empty: fail-closed (all connections denied)." << std::endl;
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
        std::cout << "[main.cpp] [Security] TLS ports auth/audio/alert/position="
                  << cfg.auth_tls_port << "/" << cfg.audio_tls_port << "/" << cfg.alert_tls_port
                  << "/" << cfg.position_tls_port
                  << ", handshake_timeout_ms=" << cfg.app_tls_handshake_timeout_ms << std::endl;
        std::cout << "[main.cpp] [Security] TLS cert file=" << cfg.app_tls_cert_file << std::endl;
    }
    std::cout << "[main.cpp] [Security] Bind IP=" << cfg.app_bind_ip << std::endl;
}

void log_esp_transport_mode(const SecurityRuntimeOptions& cfg) {
    std::cout << "[main.cpp] [ESP] enabled=" << (cfg.esp_tcp_enable ? "on" : "off")
              << ", bind_ip=" << cfg.esp_tcp_bind_ip
              << ", port=" << cfg.esp_tcp_port
              << ", max_clients=" << cfg.esp_tcp_max_clients << std::endl;
    if (cfg.esp_tcp_enable) {
        if (cfg.esp_tcp_allow_ips.empty()) {
            std::cout << "[main.cpp] [ESP] SFEPS_ESP_TCP_ALLOW_IPS is empty: allow-all within bound interface."
                      << std::endl;
        } else {
            std::cout << "[main.cpp] [ESP] SFEPS_ESP_TCP_ALLOW_IPS enabled with "
                      << cfg.esp_tcp_allow_ips.size() << " IP(s)." << std::endl;
        }
    }
}

}  // namespace

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
    log_esp_transport_mode(sec_cfg);

    std::cout << "[main.cpp] [Security] auth_max_bytes=" << sec_cfg.auth_max_bytes
              << ", audio_max_bytes=" << sec_cfg.audio_max_bytes
              << ", alert_max_clients=" << sec_cfg.alert_max_clients
              << ", position_max_clients=" << sec_cfg.position_max_clients
              << ", position_tick_ms=" << sec_cfg.position_stream_tick_ms
              << ", position_stale_sec=" << sec_cfg.position_stale_seconds
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
                                 cfg.db_name_analytics.c_str());
    if (!analytics.start()) {
        std::cerr << "[Fatal] AnalyticsProcessor startup failed (fail-closed)." << std::endl;
        return -1;
    }

    EspManager::Config esp_cfg;
    esp_cfg.enabled = sec_cfg.esp_tcp_enable;
    esp_cfg.bind_ip = sec_cfg.esp_tcp_bind_ip;
    esp_cfg.port = sec_cfg.esp_tcp_port;
    esp_cfg.max_clients = sec_cfg.esp_tcp_max_clients;
    esp_cfg.allow_ips = sec_cfg.esp_tcp_allow_ips;
    EspManager esp_manager(std::move(esp_cfg));
    analytics.setFraudBBoxCallback([&esp_manager](const AnalyticsProcessor::FraudBBoxPayload& payload) {
        EspManager::FraudBboxPayload esp_payload;
        esp_payload.object_id = payload.object_id;
        esp_payload.card_age_text = payload.card_age_text;
        esp_payload.age_group = payload.age_group;
        esp_payload.left = payload.left;
        esp_payload.top = payload.top;
        esp_payload.right = payload.right;
        esp_payload.bottom = payload.bottom;
        esp_manager.publishFraudBbox(esp_payload);
    });
    if (sec_cfg.esp_tcp_enable && !esp_manager.start(g_running)) {
        std::cerr << "[Fatal] ESP manager startup failed." << std::endl;
        analytics.stop();
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

    std::thread t_auth(run_login_auth, std::ref(g_running), std::cref(cfg), std::cref(sec_cfg));
    std::thread t_audio(run_audio_receiver, std::ref(g_running), std::cref(sec_cfg));
    std::thread t_alert(run_fraud_notifier, std::ref(g_running), std::cref(sec_cfg));
    std::thread t_position(run_position_stream_service, std::ref(g_running), std::cref(sec_cfg),
                           std::ref(analytics));

    RfidMonitor rfid_monitor(g_running, analytics);
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
    esp_manager.stop();

    if (t_test_ping.joinable()) t_test_ping.join();
    if (t_rfid.joinable()) t_rfid.join();
    if (t_position.joinable()) t_position.join();
    if (t_alert.joinable()) t_alert.join();
    if (t_audio.joinable()) t_audio.join();
    if (t_auth.joinable()) t_auth.join();
    if (t_db_cleanup.joinable()) t_db_cleanup.join();
    if (t_file_cleanup.joinable()) t_file_cleanup.join();

    analytics.stop();

    std::cout << "[main.cpp] [System] server stopped." << std::endl;
    return 0;
}
