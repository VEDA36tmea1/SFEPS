#ifndef RECORDER_H
#define RECORDER_H

#include <string>
#include <atomic>
#include "log.h"
#include "analytics.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
}

inline constexpr const char* VIDEO_SAVE_DIR = "/home/iam/SFEPS/videos";

class RTSPRecorder {
public:
    RTSPRecorder(DBLogger& logger, std::atomic<bool>& running_flag, AnalyticsProcessor& analytics);
    ~RTSPRecorder();
    void run();

private:
    DBLogger& logger;
    std::atomic<bool>& running_flag;
    AnalyticsProcessor& analytics;
    AVFormatContext *input_ctx = nullptr, *output_ctx = nullptr;
    int video_stream_idx = -1, meta_stream_idx = -1;
    time_t start_time = 0;
    std::string current_filename;
    int64_t last_dts = AV_NOPTS_VALUE; 
    
    // [타임스탬프 리셋용 변수]
    int64_t start_dts_offset = AV_NOPTS_VALUE; 
    bool is_first_packet = true;

    bool connect_and_record();
    void cleanup();
    bool open_output_file(AVCodecParameters* video_par);
    void close_current_file();
};

#endif
