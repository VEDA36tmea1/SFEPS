#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "app_services.h"
#include "alert.h"
#include "analytics.h"
#include "auth.h"
#include "cleanup.h"
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
    std::string output = "[" + title + "]\n";
    for (const auto& line : lines) {
        output += line;
        output.push_back('\n');
    }
    output += "--------------------\n";
    (void)::write(STDOUT_FILENO, output.c_str(), output.size());
}

int extract_port_number(const std::string& line) {
    const std::size_t colon_pos = line.rfind(':');
    if (colon_pos == std::string::npos || colon_pos + 1 >= line.size()) {
        return -1;
    }

    std::size_t end_pos = colon_pos + 1;
    while (end_pos < line.size() && std::isdigit(static_cast<unsigned char>(line[end_pos]))) {
        ++end_pos;
    }
    if (end_pos == colon_pos + 1) {
        return -1;
    }

    try {
        return std::stoi(line.substr(colon_pos + 1, end_pos - (colon_pos + 1)));
    } catch (...) {
        return -1;
    }
}

std::unordered_set<int> capture_listening_ports() {
    constexpr const char* kCommand =
        "sh -lc \"ss -H -tln 2>/dev/null | awk '{print $4}' || netstat -tln 2>/dev/null | awk 'NR>2 {print $4}'\"";

    std::unordered_set<int> ports;
    FILE* pipe = popen(kCommand, "r");
    if (pipe == nullptr) return ports;

    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        if (line.empty()) continue;

        const int port = extract_port_number(line);
        if ((port >= 5555 && port <= 5559) || (port >= 6555 && port <= 6559)) {
            ports.insert(port);
        }
    }
    pclose(pipe);
    return ports;
}

std::vector<int> expected_listening_ports(const SecurityRuntimeOptions& sec_cfg) {
    std::vector<int> ports;
    if (sec_cfg.app_plaintext_enable) {
        ports.push_back(app_services_shared::kAuthPort);
        ports.push_back(app_services_shared::kAudioPort);
        ports.push_back(app_services_shared::kAlertPort);
        ports.push_back(app_services_shared::kPositionPort);
        ports.push_back(sec_cfg.video_catalog_port);
    }
    if (sec_cfg.app_tls_enable) {
        ports.push_back(sec_cfg.auth_tls_port);
        ports.push_back(sec_cfg.audio_tls_port);
        ports.push_back(sec_cfg.alert_tls_port);
        ports.push_back(sec_cfg.position_tls_port);
        ports.push_back(sec_cfg.video_catalog_tls_port);
    }
    return ports;
}

void log_listening_ports(const SecurityRuntimeOptions& sec_cfg, std::atomic<bool>& running_flag) {
    std::unordered_set<int> open_ports;
    const std::vector<int> expected_ports = expected_listening_ports(sec_cfg);

    for (int attempt = 0; attempt < 50 && running_flag.load(); ++attempt) {
        open_ports = capture_listening_ports();
        bool all_ready = !expected_ports.empty();
        for (const int port : expected_ports) {
            if (open_ports.count(port) == 0) {
                all_ready = false;
                break;
            }
        }
        if (all_ready || (!open_ports.empty() && expected_ports.empty())) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::vector<std::string> lines;
    for (const int port : expected_ports) {
        if (open_ports.count(port) > 0) {
            lines.push_back("포트 " + std::to_string(port) + " : LISTEN");
        }
    }
    if (lines.empty()) {
        lines.push_back("조회 결과 없음");
    }
    print_section_log("포트 리스닝 상태", lines);
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
                           std::ref(analytics));
    std::thread t_video_catalog(run_video_catalog_service, std::ref(g_running), std::cref(cfg),
                                std::cref(sec_cfg));

    RfidMonitor rfid_monitor(g_running, analytics);
    std::thread t_rfid(&RfidMonitor::start, &rfid_monitor);

    std::cout << "서버 정상 가동" << std::endl;
    log_listening_ports(sec_cfg, g_running);

    RTSPRecorder recorder(logger, g_running, analytics);
    recorder.run();

    g_running = false;

    close_alert_client_connections();

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
