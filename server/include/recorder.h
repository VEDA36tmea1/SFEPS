#ifndef RECORDER_H
#define RECORDER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <atomic>
#include <ctime>
#include <limits>

class DBLogger;
class AnalyticsProcessor;
struct AVFormatContext;
struct AVCodecParameters;

inline constexpr const char* VIDEO_SAVE_DIR = "/home/iam/SFEPS/videos";

class RTSPRecorder {
public:
    RTSPRecorder(DBLogger& logger, std::atomic<bool>& running_flag, AnalyticsProcessor& analytics);
    ~RTSPRecorder();
    void run();

private:
    static constexpr std::int64_t kNoPts = std::numeric_limits<std::int64_t>::min();

    DBLogger& logger;
    std::atomic<bool>& running_flag;
    AnalyticsProcessor& analytics;
    AVFormatContext *input_ctx = nullptr, *output_ctx = nullptr;
    int video_stream_idx = -1, meta_stream_idx = -1;
    time_t start_time = 0;
    std::string current_filename;
    int64_t last_dts = kNoPts;
    
    // [타임스탬프 리셋용 변수]
    int64_t start_dts_offset = kNoPts;
    bool is_first_packet = true;
    std::string meta_xml_buffer;
    std::size_t meta_xml_read_pos = 0;
    std::uint64_t meta_xml_extracted_docs = 0;
    std::uint64_t meta_xml_dropped_docs = 0;
    std::uint64_t meta_xml_dropped_bytes = 0;

    bool connect_and_record();
    void cleanup();
    bool open_output_file(AVCodecParameters* video_par);
    void close_current_file();
    void reset_meta_xml_reassembly();
    void compact_meta_xml_buffer();
    void process_meta_xml_chunk(const std::uint8_t* data,
                                std::size_t len,
                                std::size_t meta_xml_buffer_max,
                                std::size_t meta_xml_doc_max_bytes,
                                std::size_t drop_log_interval);
};

#endif
