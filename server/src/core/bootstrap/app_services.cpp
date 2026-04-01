#include "app_services.h"

#include "services/app_services_impl.h"

void run_audio_receiver(std::atomic<bool>& running, const SecurityRuntimeOptions& sec_cfg) {
    app_services_impl::run_audio_receiver_impl(running, sec_cfg);
}

void play_local_rfid_tag_tone() {
    app_services_impl::play_local_rfid_tag_tone_impl();
}

void run_fraud_notifier(std::atomic<bool>& running, const SecurityRuntimeOptions& sec_cfg) {
    app_services_impl::run_fraud_notifier_impl(running, sec_cfg);
}

void run_video_catalog_service(std::atomic<bool>& running,
                               const RuntimeConfig& cfg,
                               const SecurityRuntimeOptions& sec_cfg) {
    app_services_impl::run_video_catalog_service_impl(running, cfg, sec_cfg);
}

void run_position_stream_service(std::atomic<bool>& running,
                                 const SecurityRuntimeOptions& sec_cfg,
                                 AnalyticsProcessor& analytics) {
    app_services_impl::run_position_stream_service_impl(running, sec_cfg, analytics);
}

void run_login_auth(std::atomic<bool>& running,
                    const RuntimeConfig& cfg,
                    const SecurityRuntimeOptions& sec_cfg) {
    app_services_impl::run_login_auth_impl(running, cfg, sec_cfg);
}
