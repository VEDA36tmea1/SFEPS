#include "security_runtime.h"

#include <arpa/inet.h>
#include <unistd.h>

#include <iostream>
#include <unordered_set>

#include "env_utils.h"
#include "text_utils.h"

namespace {

constexpr int kAuthPort = 5555;
constexpr int kAudioPort = 5556;
constexpr int kAlertPort = 5557;
constexpr int kPositionPort = 5558;

constexpr const char* kConfigLogPrefix = "[main.cpp] [Config]";

}  // namespace

SecurityRuntimeOptions load_security_runtime_options() {
    SecurityRuntimeOptions cfg;

    cfg.auth_allow_ips = parse_allowlist_env("SFEPS_AUTH_ALLOW_IPS");
    cfg.audio_allow_ips = parse_allowlist_env("SFEPS_AUDIO_ALLOW_IPS");
    cfg.alert_allow_ips = parse_allowlist_env("SFEPS_ALERT_ALLOW_IPS");
    cfg.auth_max_bytes = load_env_size_t("SFEPS_AUTH_MAX_BYTES", 256, 1, kConfigLogPrefix);
    cfg.audio_max_bytes =
        load_env_size_t("SFEPS_AUDIO_MAX_BYTES", 4 * 1024 * 1024, 1024, kConfigLogPrefix);
    cfg.alert_max_clients =
        load_env_size_t("SFEPS_ALERT_MAX_CLIENTS", 64, 1, kConfigLogPrefix);
    cfg.position_max_clients =
        load_env_size_t("SFEPS_POSITION_MAX_CLIENTS", 64, 1, kConfigLogPrefix);
    cfg.video_max_clients =
        load_env_size_t("SFEPS_VIDEO_MAX_CLIENTS", 32, 1, kConfigLogPrefix);
    cfg.socket_read_timeout_ms =
        load_env_int("SFEPS_SOCKET_READ_TIMEOUT_MS", 5000, 1, kConfigLogPrefix);
    cfg.position_stream_tick_ms =
        load_env_int("SFEPS_POSITION_TICK_MS", 100, 1, kConfigLogPrefix);
    cfg.position_min_send_ms =
        load_env_int("SFEPS_POSITION_MIN_SEND_MS", 1000, 1, kConfigLogPrefix);
    cfg.auth_deauth_grace_ms =
        load_env_int("SFEPS_AUTH_DEAUTH_GRACE_MS", 3000, 0, kConfigLogPrefix);
    cfg.position_stale_seconds =
        load_env_size_t("SFEPS_POSITION_STALE_SEC", 3, 1, kConfigLogPrefix);

    cfg.app_tls_enable = load_env_bool("SFEPS_APP_TLS_ENABLE", false, kConfigLogPrefix);
    cfg.app_plaintext_enable = load_env_bool("SFEPS_APP_PLAINTEXT_ENABLE", true, kConfigLogPrefix);
    cfg.auth_tls_port = load_env_port("SFEPS_AUTH_TLS_PORT", 6555, kConfigLogPrefix);
    cfg.audio_tls_port = load_env_port("SFEPS_AUDIO_TLS_PORT", 6556, kConfigLogPrefix);
    cfg.alert_tls_port = load_env_port("SFEPS_ALERT_TLS_PORT", 6557, kConfigLogPrefix);
    cfg.position_tls_port = load_env_port("SFEPS_POSITION_TLS_PORT", 6558, kConfigLogPrefix);
    cfg.video_catalog_port = load_env_port("SFEPS_VIDEO_CATALOG_PORT", 5559, kConfigLogPrefix);
    cfg.video_catalog_tls_port =
        load_env_port("SFEPS_VIDEO_CATALOG_TLS_PORT", 6559, kConfigLogPrefix);
    cfg.app_tls_handshake_timeout_ms =
        load_env_int("SFEPS_APP_TLS_HANDSHAKE_TIMEOUT_MS", 3000, 1, kConfigLogPrefix);

    const std::string bind_ip = load_env_string("SFEPS_APP_BIND_IP");
    if (!bind_ip.empty()) {
        cfg.app_bind_ip = trim_copy(bind_ip);
    }

    cfg.app_tls_cert_file = load_env_string("SFEPS_APP_TLS_CERT_FILE");
    cfg.app_tls_key_file = load_env_string("SFEPS_APP_TLS_KEY_FILE");

    const std::string video_http_base_url = load_env_string("SFEPS_VIDEO_HTTP_BASE_URL");
    if (!video_http_base_url.empty()) {
        cfg.video_http_base_url = trim_copy(video_http_base_url);
    }
    const std::string fraud_image_http_base_url =
        load_env_string("SFEPS_FRAUD_IMAGE_HTTP_BASE_URL");
    if (!fraud_image_http_base_url.empty()) {
    cfg.fraud_image_http_base_url = trim_copy(fraud_image_http_base_url);
    }
    cfg.video_retention_sec =
        load_env_size_t("SFEPS_VIDEO_RETENTION_SEC", 86400, 1, kConfigLogPrefix);
    cfg.video_max_storage_bytes =
        load_env_size_t("SFEPS_VIDEO_MAX_STORAGE_BYTES", 5368709120ULL, 1, kConfigLogPrefix);
    cfg.pending_image_retention_sec =
        load_env_size_t("SFEPS_PENDING_IMAGE_RETENTION_SEC", 120, 1, kConfigLogPrefix);
    cfg.fraud_image_retention_sec =
        load_env_size_t("SFEPS_FRAUD_IMAGE_RETENTION_SEC", 86400, 1, kConfigLogPrefix);

    cfg.esp_tcp_enable = load_env_bool("SFEPS_ESP_TCP_ENABLE", false, kConfigLogPrefix);
    cfg.esp_tcp_port = load_env_port("SFEPS_ESP_TCP_PORT", 5565, kConfigLogPrefix);
    cfg.esp_tcp_max_clients =
        load_env_size_t("SFEPS_ESP_TCP_MAX_CLIENTS", 4, 1, kConfigLogPrefix);
    cfg.esp_tcp_allow_ips = parse_allowlist_env("SFEPS_ESP_TCP_ALLOW_IPS");

    const std::string esp_bind_ip = load_env_string("SFEPS_ESP_TCP_BIND_IP");
    if (!esp_bind_ip.empty()) {
        cfg.esp_tcp_bind_ip = trim_copy(esp_bind_ip);
    }
    cfg.esp_test_track_pos_enable =
        load_env_bool("SFEPS_ESP_TEST_TRACK_POS_ENABLE", false, kConfigLogPrefix);
    cfg.esp_test_track_pos_interval_sec = load_env_int(
        "SFEPS_ESP_TEST_TRACK_POS_INTERVAL_SEC", 5, 1, kConfigLogPrefix);
    const std::string esp_test_track_pos_object_id =
        load_env_string("SFEPS_ESP_TEST_TRACK_POS_OBJECT_ID", "ESP-TEST-01");
    if (!esp_test_track_pos_object_id.empty()) {
        cfg.esp_test_track_pos_object_id = trim_copy(esp_test_track_pos_object_id);
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
        cfg.video_catalog_tls_port,
    };
    if (tls_ports.size() != 5) {
        err =
            "invalid TLS port config: SFEPS_AUTH/AUDIO/ALERT/POSITION/VIDEO_CATALOG_TLS_PORT "
            "must be unique";
        return false;
    }

    if (cfg.app_plaintext_enable) {
        std::unordered_set<int> plain_ports = {
            kAuthPort, kAudioPort, kAlertPort, kPositionPort, cfg.video_catalog_port};
        if (plain_ports.size() != 5) {
            err = "invalid plaintext port config: SFEPS_VIDEO_CATALOG_PORT must not collide with "
                  "5555/5556/5557/5558";
            return false;
        }
        if (plain_ports.count(cfg.auth_tls_port) > 0 || plain_ports.count(cfg.audio_tls_port) > 0 ||
            plain_ports.count(cfg.alert_tls_port) > 0 ||
            plain_ports.count(cfg.position_tls_port) > 0 ||
            plain_ports.count(cfg.video_catalog_tls_port) > 0) {
            err =
                "invalid TLS port config: TLS ports collide with plaintext ports "
                "(5555/5556/5557/5558/5559)";
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

    if (allowlist.find("*") != allowlist.end()) {
        std::cout << "[main.cpp] [Security] " << env_name
                  << " allow-all mode enabled." << std::endl;
        return;
    }

    std::cout << "[main.cpp] [Security] " << env_name << " 활성화됨: " << allowlist.size()
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
        std::cout << "[main.cpp] [Security] TLS ports auth/audio/alert/position/video_catalog="
                  << cfg.auth_tls_port << "/" << cfg.audio_tls_port << "/" << cfg.alert_tls_port
                  << "/" << cfg.position_tls_port << "/" << cfg.video_catalog_tls_port
                  << ", handshake_timeout_ms=" << cfg.app_tls_handshake_timeout_ms << std::endl;
        std::cout << "[main.cpp] [Security] TLS cert file=" << cfg.app_tls_cert_file << std::endl;
    }

    if (cfg.app_plaintext_enable) {
        std::cout << "[main.cpp] [Security] Plain ports auth/audio/alert/position/video_catalog="
                  << kAuthPort << "/" << kAudioPort << "/" << kAlertPort << "/"
                  << kPositionPort << "/" << cfg.video_catalog_port << std::endl;
    }
    std::cout << "[main.cpp] [Security] Bind IP=" << cfg.app_bind_ip << std::endl;
}

void log_esp_transport_mode(const SecurityRuntimeOptions& cfg) {
    std::cout << "[main.cpp] [ESP] 활성화=" << (cfg.esp_tcp_enable ? "on" : "off")
              << ", bind_ip=" << cfg.esp_tcp_bind_ip
              << ", port=" << cfg.esp_tcp_port
              << ", max_clients=" << cfg.esp_tcp_max_clients << std::endl;
    if (cfg.esp_tcp_enable) {
        if (cfg.esp_tcp_allow_ips.empty()) {
            std::cout << "[main.cpp] [ESP] SFEPS_ESP_TCP_ALLOW_IPS is empty: allow-all within bound interface."
                      << std::endl;
        } else {
            std::cout << "[main.cpp] [ESP] SFEPS_ESP_TCP_ALLOW_IPS 활성화됨: "
                      << cfg.esp_tcp_allow_ips.size() << " IP(s)." << std::endl;
        }
    }
}
