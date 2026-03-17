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
#include <mutex>
#include <stdexcept>
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
constexpr const char* RFID_IMAGE_SOURCE_PATH =
    "/home/iam/SFEPS/Camera/image_processing/3_best_shot.jpg";
constexpr const char* EVENT_IMAGE_BASE_DIR = "/home/iam/SFEPS/event_images";
constexpr const char* EVENT_IMAGE_PENDING_DIR = "/home/iam/SFEPS/event_images/pending";
constexpr const char* EVENT_IMAGE_FRAUD_DIR = "/home/iam/SFEPS/event_images/fraud";
constexpr const char* EVENT_IMAGE_FAILED_DIR = "/home/iam/SFEPS/event_images/failed";

std::atomic<bool> g_running(true);

namespace {

struct EventImageRegistry {
    std::mutex mutex;
    std::unordered_map<std::string, fs::path> pending_by_object_id;
};

EventImageRegistry& event_image_registry() {
    static EventImageRegistry registry;
    return registry;
}

std::uint64_t unix_epoch_ms_now() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string sanitize_filename_token(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (std::isalnum(c) != 0 || c == '-' || c == '_') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "unknown" : out;
}

fs::path make_unique_path(const fs::path& preferred) {
    if (!fs::exists(preferred)) return preferred;

    const fs::path dir = preferred.parent_path();
    const std::string stem = preferred.stem().string();
    const std::string ext = preferred.extension().string();
    for (int i = 0; i < 1000; ++i) {
        fs::path candidate = dir / (stem + "_" + std::to_string(unix_epoch_ms_now()) + "_" +
                                    std::to_string(i) + ext);
        if (!fs::exists(candidate)) return candidate;
    }
    return dir / (stem + "_" + std::to_string(unix_epoch_ms_now()) + "_overflow" + ext);
}

fs::path move_file_to_dir(const fs::path& source, const fs::path& target_dir) {
    const fs::path target = make_unique_path(target_dir / source.filename());
    std::error_code ec;
    fs::rename(source, target, ec);
    if (!ec) return target;

    ec.clear();
    if (fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec)) {
        std::error_code remove_ec;
        fs::remove(source, remove_ec);
        return target;
    }

    throw std::runtime_error("failed to move image file: " + source.string() + " -> " +
                             target.string() + " (" + ec.message() + ")");
}

void snapshot_rfid_image_for_object(const std::string& object_id) {
    if (object_id.empty()) return;

    auto& registry = event_image_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    try {
        const fs::path source(RFID_IMAGE_SOURCE_PATH);
        if (!fs::exists(source) || !fs::is_regular_file(source)) {
            std::cout << "[main.cpp] [RFID_IMAGE_SNAP] source image missing: " << source
                      << ", object_id=" << object_id << std::endl;
            return;
        }

        const auto existing = registry.pending_by_object_id.find(object_id);
        if (existing != registry.pending_by_object_id.end()) {
            std::error_code remove_ec;
            fs::remove(existing->second, remove_ec);
        }

        const fs::path target = make_unique_path(
            fs::path(EVENT_IMAGE_PENDING_DIR) /
            ("rfid_" + std::to_string(unix_epoch_ms_now()) + "_" +
             sanitize_filename_token(object_id) + ".jpg"));

        fs::copy_file(source, target, fs::copy_options::overwrite_existing);
        registry.pending_by_object_id[object_id] = target;
        std::cout << "[main.cpp] [RFID_IMAGE_SNAP] object_id=" << object_id
                  << ", source=" << source << ", saved=" << target << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[main.cpp] [RFID_IMAGE_SNAP] failed: object_id=" << object_id
                  << ", err=" << e.what() << std::endl;
    }
}

void finalize_outline_image_for_object(
    const AnalyticsProcessor::OutlineDecisionPayload& payload) {
    if (payload.object_id.empty()) return;

    auto& registry = event_image_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);

    const auto it = registry.pending_by_object_id.find(payload.object_id);
    if (it == registry.pending_by_object_id.end()) {
        std::cout << "[main.cpp] [" << (payload.is_fraud ? "RFID_IMAGE_KEEP" : "RFID_IMAGE_DELETE")
                  << "] no pending image: object_id=" << payload.object_id
                  << ", tag_time=" << payload.tag_time << std::endl;
        return;
    }

    const fs::path pending_path = it->second;
    registry.pending_by_object_id.erase(it);

    if (!fs::exists(pending_path)) {
        std::cout << "[main.cpp] [" << (payload.is_fraud ? "RFID_IMAGE_KEEP" : "RFID_IMAGE_DELETE")
                  << "] pending image missing on disk: object_id=" << payload.object_id
                  << ", path=" << pending_path << ", tag_time=" << payload.tag_time << std::endl;
        return;
    }

    if (!payload.is_fraud) {
        std::error_code remove_ec;
        if (fs::remove(pending_path, remove_ec)) {
            std::cout << "[main.cpp] [RFID_IMAGE_DELETE] object_id=" << payload.object_id
                      << ", path=" << pending_path << ", tag_time=" << payload.tag_time
                      << std::endl;
            return;
        }

        try {
            const fs::path moved = move_file_to_dir(pending_path, fs::path(EVENT_IMAGE_FAILED_DIR));
            std::cerr << "[main.cpp] [RFID_IMAGE_DELETE] failed to delete pending image, moved to"
                      << " failed dir: object_id=" << payload.object_id << ", moved=" << moved
                      << ", err=" << remove_ec.message() << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[main.cpp] [RFID_IMAGE_DELETE] failed: object_id=" << payload.object_id
                      << ", path=" << pending_path << ", err=" << e.what() << std::endl;
        }
        return;
    }

    try {
        const fs::path kept = move_file_to_dir(pending_path, fs::path(EVENT_IMAGE_FRAUD_DIR));
        std::cout << "[main.cpp] [RFID_IMAGE_KEEP] object_id=" << payload.object_id
                  << ", from=" << pending_path << ", to=" << kept
                  << ", tag_time=" << payload.tag_time << std::endl;
    } catch (const std::exception& keep_err) {
        try {
            const fs::path failed = move_file_to_dir(pending_path, fs::path(EVENT_IMAGE_FAILED_DIR));
            std::cerr << "[main.cpp] [RFID_IMAGE_KEEP] failed to keep in fraud dir, moved to failed"
                      << ": object_id=" << payload.object_id << ", moved=" << failed
                      << ", err=" << keep_err.what() << std::endl;
        } catch (const std::exception& failed_err) {
            std::cerr << "[main.cpp] [RFID_IMAGE_KEEP] failed: object_id=" << payload.object_id
                      << ", path=" << pending_path << ", err=" << keep_err.what()
                      << ", failed_err=" << failed_err.what() << std::endl;
        }
    }
}

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
    cfg.position_min_send_ms = load_env_int("SFEPS_POSITION_MIN_SEND_MS", 1000, 1);
    cfg.auth_deauth_grace_ms = load_env_int("SFEPS_AUTH_DEAUTH_GRACE_MS", 3000, 0);
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

int main() {
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
              << ", auth_deauth_grace_ms=" << sec_cfg.auth_deauth_grace_ms
              << ", position_stale_sec=" << sec_cfg.position_stale_seconds
              << ", socket_read_timeout_ms=" << sec_cfg.socket_read_timeout_ms << std::endl;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        if (!fs::exists(VIDEO_SAVE_DIR)) {
            fs::create_directories(VIDEO_SAVE_DIR);
        }
        fs::create_directories(EVENT_IMAGE_BASE_DIR);
        fs::create_directories(EVENT_IMAGE_PENDING_DIR);
        fs::create_directories(EVENT_IMAGE_FRAUD_DIR);
        fs::create_directories(EVENT_IMAGE_FAILED_DIR);
    } catch (const std::exception& e) {
        std::cerr << "[Fatal] Failed to create runtime media directory: " << e.what() << std::endl;
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
    analytics.setRfidPairedCallback([](const std::string& object_id) {
        snapshot_rfid_image_for_object(object_id);
    });
    analytics.setOutlineDecisionCallback(
        [](const AnalyticsProcessor::OutlineDecisionPayload& payload) {
            finalize_outline_image_for_object(payload);
        });
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
        esp_payload.age = payload.age;
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
                           std::ref(analytics), std::ref(esp_manager));

    RfidMonitor rfid_monitor(g_running, analytics);
    std::thread t_rfid(&RfidMonitor::start, &rfid_monitor);

    RTSPRecorder recorder(logger, g_running, analytics);
    recorder.run();

    g_running = false;

    close_alert_client_connections();
    esp_manager.stop();

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
