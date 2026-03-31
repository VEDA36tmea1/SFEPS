#include <atomic>
#include <chrono>
#include <csignal>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
#include "service_shared.h"

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

void print_section_log(const std::string& title, const std::vector<std::string>& lines) {
    std::cout << "[" << title << "]" << std::endl;
    for (const auto& line : lines) {
        std::cout << line << std::endl;
    }
    std::cout << "--------------------" << std::endl;
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
        return -1;
    }

    const SecurityRuntimeOptions sec_cfg = load_security_runtime_options();
    std::string sec_cfg_err;
    if (!validate_security_runtime_options(sec_cfg, sec_cfg_err)) {
        return -1;
    }

    {
        Authenticator auth_probe(cfg.db_host.c_str(), cfg.db_user.c_str(), cfg.db_pass.c_str(),
                                 cfg.db_name_analytics.c_str());
        if (!auth_probe.connect()) {
            return -1;
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::string media_dir_err;
    if (!ensure_runtime_media_dirs(media_dir_err)) {
        return -1;
    }

    DBLogger logger(cfg.db_host.c_str(), cfg.db_user.c_str(), cfg.db_pass.c_str(),
                    cfg.db_name_analytics.c_str());
    if (!logger.connect()) {
        return -1;
    }

    AnalyticsProcessor analytics(cfg.db_host.c_str(), cfg.db_user.c_str(), cfg.db_pass.c_str(),
                                 cfg.db_name_analytics.c_str());
    analytics.setRfidPairedCallback(
        [](const std::string& object_id, const std::string& tag_time) {
            snapshot_rfid_image_for_object(object_id, tag_time);
        });
    analytics.setOutlineDecisionCallback(
        [&sec_cfg](const AnalyticsProcessor::OutlineDecisionPayload& payload) {
            const bool detected_fraud = payload.is_fraud || payload.card_age_text == "0";
            const bool should_send_image_ref = detected_fraud && payload.card_age_text != "0";
            AnalyticsProcessor::OutlineDecisionPayload send_payload = payload;
            send_payload.is_fraud = detected_fraud;
            FinalizedFraudImageInfo fraud_image_info;
            bool image_ref_sent = false;
            if (should_send_image_ref &&
                finalize_outline_image_for_object(send_payload, &fraud_image_info) &&
                !sec_cfg.fraud_image_http_base_url.empty() &&
                !fraud_image_info.filename.empty()) {
                const std::string image_url = app_services_shared::join_http_url(
                    sec_cfg.fraud_image_http_base_url, fraud_image_info.filename);
                std::string message = "IMG_REF|OBJECT_ID=" +
                                      sanitize_alert_field(fraud_image_info.object_id) +
                                      "|URL=" + sanitize_alert_field(image_url) +
                                      "|TAG=" + sanitize_alert_field(fraud_image_info.tag_time) +
                                      "|NAME=" + sanitize_alert_field(fraud_image_info.filename);
                message.push_back('\n');
                send_alert_to_clients(message);
                image_ref_sent = true;
            }

            const std::string card_age_display =
                (payload.card_age_text == "0") ? "미태그" : payload.card_age_text;
            std::vector<std::string> lines;
            lines.push_back("ID         : " + payload.object_id);
            lines.push_back("카드 구분  : " + card_age_display);
            lines.push_back(std::string("상태       : ") +
                            (detected_fraud ? "부정승차 감지" : "정상승차"));
            if (detected_fraud) {
                lines.push_back("클라이언트 : 송신 완료");
            }
            print_section_log("판정 결과", lines);
        });
    if (!analytics.start()) {
        return -1;
    }

    EspManager::Config esp_cfg;
    esp_cfg.enabled = sec_cfg.esp_tcp_enable;
    esp_cfg.bind_ip = sec_cfg.esp_tcp_bind_ip;
    esp_cfg.port = sec_cfg.esp_tcp_port;
    esp_cfg.max_clients = sec_cfg.esp_tcp_max_clients;
    esp_cfg.allow_ips = sec_cfg.esp_tcp_allow_ips;
    EspManager esp_manager(std::move(esp_cfg));
    analytics.setTrackPosCallback(
        [&esp_manager](const AnalyticsProcessor::TrackPosPayload& payload) {
            EspManager::TrackPosPayload esp_payload;
            esp_payload.object_id = payload.object_id;
            esp_payload.left = payload.left;
            esp_payload.top = payload.top;
            esp_payload.right = payload.right;
            esp_payload.bottom = payload.bottom;
            esp_payload.x = payload.x;
            esp_payload.y = payload.y;
            esp_manager.publishFraudTrackPosIfIdle(esp_payload);
        });
    if (sec_cfg.esp_tcp_enable && !esp_manager.start(g_running)) {
        analytics.stop();
        return -1;
    }

    std::thread t_file_cleanup(run_file_cleanup_worker, std::ref(g_running),
                               std::string(VIDEO_SAVE_DIR),
                               static_cast<long>(sec_cfg.video_retention_sec),
                               sec_cfg.video_max_storage_bytes,
                               sec_cfg.video_storage_resume_bytes);
    std::thread t_pending_image_cleanup(run_pending_image_cleanup_worker, std::ref(g_running),
                                        std::string(pending_image_directory_path()),
                                        static_cast<long>(sec_cfg.pending_image_retention_sec));
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

    std::cout << "서버 정상 가동" << std::endl;

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
    std::cout << "서버 정상 종료" << std::endl;
    return 0;
}
