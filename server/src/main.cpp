#include <atomic>
#include <chrono>
#include <csignal>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

#include "app_services.h"
#include "alert.h"
#include "analytics.h"
#include "auth.h"
#include "cleanup.h"
#include "esp_manager.h"
#include "log.h"
#include "recorder.h"
#include "rfid_image_pipeline.h"
#include "rfid_monitor.h"
#include "runtime_config.h"
#include "security_runtime.h"

std::atomic<bool> g_running(true);

namespace {

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

std::string sanitize_alert_field(std::string value) {
    for (char& c : value) {
        if (c == '|' || c == '\n' || c == '\r') {
            c = '_';
        }
    }
    return value;
}

std::string join_http_url(const std::string& base, const std::string& filename) {
    std::string normalized_base = base;
    while (!normalized_base.empty() && normalized_base.back() == '/') {
        normalized_base.pop_back();
    }
    if (normalized_base.empty()) return filename;
    return normalized_base + "/" + filename;
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
        std::cerr << "[Fatal] 런타임 설정 오류: " << cfg_err << std::endl;
        return -1;
    }

    const SecurityRuntimeOptions sec_cfg = load_security_runtime_options();
    std::string sec_cfg_err;
    if (!validate_security_runtime_options(sec_cfg, sec_cfg_err)) {
        std::cerr << "[Fatal] 보안 설정 오류: " << sec_cfg_err << std::endl;
        return -1;
    }

    {
        Authenticator auth_probe(cfg.db_host.c_str(), cfg.db_user.c_str(), cfg.db_pass.c_str(),
                                 cfg.db_name_analytics.c_str());
        if (!auth_probe.connect()) {
            std::cerr << "[Fatal] Auth DB 시작 점검 실패(fail-closed)." << std::endl;
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
              << ", video_max_clients=" << sec_cfg.video_max_clients
              << ", position_tick_ms=" << sec_cfg.position_stream_tick_ms
              << ", auth_deauth_grace_ms=" << sec_cfg.auth_deauth_grace_ms
              << ", position_stale_sec=" << sec_cfg.position_stale_seconds
              << ", socket_read_timeout_ms=" << sec_cfg.socket_read_timeout_ms << std::endl;
    std::cout << "[main.cpp] [Security] fraud_image_http_base_url="
              << sec_cfg.fraud_image_http_base_url
              << ", fraud_image_retention_sec=" << sec_cfg.fraud_image_retention_sec << std::endl;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::string media_dir_err;
    if (!ensure_runtime_media_dirs(media_dir_err)) {
        std::cerr << "[Fatal] Failed to create runtime media directory: " << media_dir_err
                  << std::endl;
        return -1;
    }

    DBLogger logger(cfg.db_host.c_str(), cfg.db_user.c_str(), cfg.db_pass.c_str(),
                    cfg.db_name_analytics.c_str());
    if (!logger.connect()) {
        std::cerr << "[Fatal] DBLogger 시작 실패(fail-closed)." << std::endl;
        return -1;
    }

    AnalyticsProcessor analytics(cfg.db_host.c_str(), cfg.db_user.c_str(), cfg.db_pass.c_str(),
                                 cfg.db_name_analytics.c_str());
    analytics.setRfidPairedCallback(
        [](const std::string& object_id) { snapshot_rfid_image_for_object(object_id); });
    analytics.setOutlineDecisionCallback(
        [&sec_cfg](const AnalyticsProcessor::OutlineDecisionPayload& payload) {
            FinalizedFraudImageInfo fraud_image_info;
            if (!finalize_outline_image_for_object(payload, &fraud_image_info)) {
                return;
            }
            if (sec_cfg.fraud_image_http_base_url.empty()) {
                std::cerr << "[main.cpp] [RFID_IMAGE_REF_SEND] 생략: 비어 있음 "
                             "SFEPS_FRAUD_IMAGE_HTTP_BASE_URL, object_id="
                          << payload.object_id << std::endl;
                return;
            }
            if (fraud_image_info.filename.empty()) {
                std::cerr << "[main.cpp] [RFID_IMAGE_REF_SEND] 생략: 파일명 누락, object_id="
                          << payload.object_id << std::endl;
                return;
            }

            const std::string url =
                join_http_url(sec_cfg.fraud_image_http_base_url, fraud_image_info.filename);
            std::string message = "IMG_REF|OBJECT_ID=" +
                                  sanitize_alert_field(fraud_image_info.object_id) +
                                  "|URL=" + sanitize_alert_field(url) +
                                  "|TAG=" + sanitize_alert_field(fraud_image_info.tag_time) +
                                  "|NAME=" + sanitize_alert_field(fraud_image_info.filename);
            message.push_back('\n');
            send_alert_to_clients(message);
            std::cout << "[main.cpp] [RFID_IMAGE_REF_SEND] object_id="
                      << fraud_image_info.object_id << ", tag_time=" << fraud_image_info.tag_time
                      << ", name=" << fraud_image_info.filename << ", url=" << url << std::endl;
        });
    if (!analytics.start()) {
        std::cerr << "[Fatal] AnalyticsProcessor 시작 실패(fail-closed)." << std::endl;
        return -1;
    }

    EspManager::Config esp_cfg;
    esp_cfg.enabled = sec_cfg.esp_tcp_enable;
    esp_cfg.bind_ip = sec_cfg.esp_tcp_bind_ip;
    esp_cfg.port = sec_cfg.esp_tcp_port;
    esp_cfg.max_clients = sec_cfg.esp_tcp_max_clients;
    esp_cfg.allow_ips = sec_cfg.esp_tcp_allow_ips;
    EspManager esp_manager(std::move(esp_cfg));
    analytics.setFraudBBoxCallback(
        [&esp_manager](const AnalyticsProcessor::FraudBBoxPayload& payload) {
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
        std::cerr << "[Fatal] ESP manager 시작 실패." << std::endl;
        analytics.stop();
        return -1;
    }

    std::thread t_file_cleanup(run_file_cleanup_worker, std::ref(g_running),
                               std::string(VIDEO_SAVE_DIR), 300);
    std::thread t_pending_image_cleanup(run_pending_image_cleanup_worker, std::ref(g_running),
                                        std::string(pending_image_directory_path()),
                                        static_cast<long>(sec_cfg.fraud_image_retention_sec));
    std::thread t_fraud_image_cleanup(run_fraud_image_cleanup_worker, std::ref(g_running),
                                      std::string(fraud_image_directory_path()),
                                      static_cast<long>(sec_cfg.fraud_image_retention_sec));

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
    std::thread t_video_catalog(run_video_catalog_service, std::ref(g_running), std::cref(cfg),
                                std::cref(sec_cfg));

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
    if (t_video_catalog.joinable()) t_video_catalog.join();
    if (t_db_cleanup.joinable()) t_db_cleanup.join();
    if (t_pending_image_cleanup.joinable()) t_pending_image_cleanup.join();
    if (t_fraud_image_cleanup.joinable()) t_fraud_image_cleanup.join();
    if (t_file_cleanup.joinable()) t_file_cleanup.join();

    analytics.stop();

    std::cout << "[main.cpp] [System] 서버 종료." << std::endl;
    return 0;
}
