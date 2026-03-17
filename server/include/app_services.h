#ifndef APP_SERVICES_H
#define APP_SERVICES_H

#include <atomic>
#include <cstddef>
#include <string>
#include <unordered_set>

#include "runtime_config.h"

class AnalyticsProcessor;
class EspManager;

struct SecurityRuntimeOptions {
    std::unordered_set<std::string> auth_allow_ips;
    std::unordered_set<std::string> audio_allow_ips;
    std::unordered_set<std::string> alert_allow_ips;
    std::size_t auth_max_bytes = 256;
    std::size_t audio_max_bytes = 4 * 1024 * 1024;
    std::size_t alert_max_clients = 64;
    std::size_t position_max_clients = 64;
    int socket_read_timeout_ms = 5000;
    int position_stream_tick_ms = 100;
    int position_min_send_ms = 500;
    int auth_deauth_grace_ms = 3000;
    std::size_t position_stale_seconds = 3;

    bool app_tls_enable = false;
    bool app_plaintext_enable = true;
    int auth_tls_port = 6555;
    int audio_tls_port = 6556;
    int alert_tls_port = 6557;
    int position_tls_port = 6558;
    std::string app_tls_cert_file;
    std::string app_tls_key_file;
    int app_tls_handshake_timeout_ms = 3000;
    std::string app_bind_ip = "0.0.0.0";

    bool esp_tcp_enable = false;
    int esp_tcp_port = 5565;
    std::size_t esp_tcp_max_clients = 4;
    std::string esp_tcp_bind_ip = "192.168.4.1";
    std::unordered_set<std::string> esp_tcp_allow_ips;
};

void run_audio_receiver(std::atomic<bool>& running, const SecurityRuntimeOptions& sec_cfg);
void run_fraud_notifier(std::atomic<bool>& running, const SecurityRuntimeOptions& sec_cfg);
void run_position_stream_service(std::atomic<bool>& running,
                                 const SecurityRuntimeOptions& sec_cfg,
                                 AnalyticsProcessor& analytics,
                                 EspManager& esp_manager);
void run_login_auth(std::atomic<bool>& running,
                    const RuntimeConfig& cfg,
                    const SecurityRuntimeOptions& sec_cfg);

#endif
