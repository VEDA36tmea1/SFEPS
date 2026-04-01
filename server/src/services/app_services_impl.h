#ifndef APP_SERVICES_IMPL_H
#define APP_SERVICES_IMPL_H

#include <atomic>

#include "app_services.h"

class AnalyticsProcessor;

namespace app_services_impl {

void run_audio_receiver_impl(std::atomic<bool>& running, const SecurityRuntimeOptions& sec_cfg);
void play_local_rfid_tag_tone_impl();
void run_fraud_notifier_impl(std::atomic<bool>& running, const SecurityRuntimeOptions& sec_cfg);
void run_position_stream_service_impl(std::atomic<bool>& running,
                                      const SecurityRuntimeOptions& sec_cfg,
                                      AnalyticsProcessor& analytics);
void run_login_auth_impl(std::atomic<bool>& running,
                         const RuntimeConfig& cfg,
                         const SecurityRuntimeOptions& sec_cfg);
void run_video_catalog_service_impl(std::atomic<bool>& running,
                                    const RuntimeConfig& cfg,
                                    const SecurityRuntimeOptions& sec_cfg);

}  // namespace app_services_impl

#endif
