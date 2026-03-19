#ifndef RFID_IMAGE_PIPELINE_H
#define RFID_IMAGE_PIPELINE_H

#include <string>

#include "analytics.h"

struct FinalizedFraudImageInfo {
    std::string object_id;
    std::string tag_time;
    std::string filename;
    std::string absolute_path;
};

bool ensure_runtime_media_dirs(std::string& err);
void snapshot_rfid_image_for_object(const std::string& object_id);
bool finalize_outline_image_for_object(const AnalyticsProcessor::OutlineDecisionPayload& payload,
                                       FinalizedFraudImageInfo* out_fraud_image_info = nullptr);
const char* pending_image_directory_path();
const char* fraud_image_directory_path();

#endif
