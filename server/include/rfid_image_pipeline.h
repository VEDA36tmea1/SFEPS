#ifndef RFID_IMAGE_PIPELINE_H
#define RFID_IMAGE_PIPELINE_H

#include <string>

#include "analytics.h"

bool ensure_runtime_media_dirs(std::string& err);
void snapshot_rfid_image_for_object(const std::string& object_id);
void finalize_outline_image_for_object(const AnalyticsProcessor::OutlineDecisionPayload& payload);

#endif
