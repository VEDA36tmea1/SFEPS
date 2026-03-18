#include "recorder.h"

#include "analytics.h"
#include "env_utils.h"
#include "log.h"
#include "sample_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iostream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
}

namespace {
constexpr const char* kDefaultRtspUrl = "rtsp://127.0.0.1:8554/cam1";
constexpr int kSegmentDurationSec = 60;
constexpr const char* kXmlDeclStart = "<?xml";
constexpr const char* kMetadataStreamStartTag = "<tt:MetadataStream";
constexpr const char* kMetadataStreamStartTagNoNs = "<MetadataStream";
constexpr const char* kMetadataStreamEndTag = "</tt:MetadataStream>";
constexpr const char* kMetadataStreamEndTagNoNs = "</MetadataStream>";
constexpr std::size_t kMetaXmlCompactionThreshold = 64 * 1024;
constexpr std::size_t kMetaXmlStartOverlapBytes = 32;
constexpr const char* kRecorderLogPrefix = "[recorder.cpp]";

struct RecorderRuntimeLimits {
    std::size_t max_meta_packet_bytes = 65536;
    std::size_t bad_meta_streak_limit = 20;
    std::size_t drop_log_interval = 100;
    std::size_t meta_xml_buffer_max = 1024 * 1024;
    std::size_t meta_xml_doc_max_bytes = 256 * 1024;
};

RecorderRuntimeLimits load_recorder_runtime_limits() {
    RecorderRuntimeLimits out;
    out.max_meta_packet_bytes =
        load_env_size_t("SFEPS_META_MAX_PACKET_BYTES", 65536, 1, kRecorderLogPrefix);
    out.bad_meta_streak_limit =
        load_env_size_t("SFEPS_META_BAD_STREAK_LIMIT", 20, 1, kRecorderLogPrefix);
    out.drop_log_interval =
        load_env_size_t("SFEPS_DROP_LOG_INTERVAL", 100, 1, kRecorderLogPrefix);
    out.meta_xml_buffer_max =
        load_env_size_t("SFEPS_META_XML_BUFFER_MAX", 1024 * 1024, 1024, kRecorderLogPrefix);
    out.meta_xml_doc_max_bytes =
        load_env_size_t("SFEPS_META_XML_DOC_MAX_BYTES", 256 * 1024, 1024, kRecorderLogPrefix);
    if (out.meta_xml_doc_max_bytes > out.meta_xml_buffer_max) {
        std::cout << "[recorder.cpp] Invalid env relationship: SFEPS_META_XML_DOC_MAX_BYTES("
                  << out.meta_xml_doc_max_bytes << ") > SFEPS_META_XML_BUFFER_MAX("
                  << out.meta_xml_buffer_max << "), clamping doc max to buffer max."
                  << std::endl;
        out.meta_xml_doc_max_bytes = out.meta_xml_buffer_max;
    }
    return out;
}

std::size_t choose_earlier_pos(std::size_t lhs, std::size_t rhs) {
    if (lhs == std::string::npos) return rhs;
    if (rhs == std::string::npos) return lhs;
    return (lhs < rhs) ? lhs : rhs;
}

std::size_t find_next_metadata_start(const std::string& buffer, std::size_t from) {
    std::size_t pos = buffer.find(kXmlDeclStart, from);
    pos = choose_earlier_pos(pos, buffer.find(kMetadataStreamStartTag, from));
    pos = choose_earlier_pos(pos, buffer.find(kMetadataStreamStartTagNoNs, from));
    return pos;
}

bool find_next_metadata_end(const std::string& buffer,
                            std::size_t from,
                            std::size_t& end_pos,
                            std::size_t& end_len) {
    const std::size_t namespaced = buffer.find(kMetadataStreamEndTag, from);
    const std::size_t plain = buffer.find(kMetadataStreamEndTagNoNs, from);
    const std::size_t pos = choose_earlier_pos(namespaced, plain);
    if (pos == std::string::npos) return false;

    end_pos = pos;
    end_len = (pos == namespaced) ? std::strlen(kMetadataStreamEndTag)
                                  : std::strlen(kMetadataStreamEndTagNoNs);
    return true;
}

} // namespace

namespace {
int ffmpeg_interrupt_cb(void* opaque) {
    auto* running = static_cast<std::atomic<bool>*>(opaque);
    return (running && !running->load()) ? 1 : 0;
}
}

// [헬퍼] 시간 문자열
static std::string get_time_str() {
    auto t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", std::localtime(&t));
    return std::string(buf);
}

RTSPRecorder::RTSPRecorder(DBLogger& l, std::atomic<bool>& f, AnalyticsProcessor& a) : logger(l), running_flag(f), analytics(a) {}
RTSPRecorder::~RTSPRecorder() { cleanup(); }

void RTSPRecorder::cleanup() {
    close_current_file();
    if (input_ctx) avformat_close_input(&input_ctx);
    reset_meta_xml_reassembly();
}

void RTSPRecorder::reset_meta_xml_reassembly() {
    meta_xml_buffer.clear();
    meta_xml_read_pos = 0;
    meta_xml_extracted_docs = 0;
    meta_xml_dropped_docs = 0;
    meta_xml_dropped_bytes = 0;
}

void RTSPRecorder::compact_meta_xml_buffer() {
    if (meta_xml_read_pos == 0) return;
    if (meta_xml_read_pos >= meta_xml_buffer.size()) {
        meta_xml_buffer.clear();
        meta_xml_read_pos = 0;
        return;
    }
    meta_xml_buffer.erase(0, meta_xml_read_pos);
    meta_xml_read_pos = 0;
}

void RTSPRecorder::process_meta_xml_chunk(const std::uint8_t* data,
                                          std::size_t len,
                                          std::size_t meta_xml_buffer_max,
                                          std::size_t meta_xml_doc_max_bytes,
                                          std::size_t drop_log_interval) {
    if (data == nullptr || len == 0) return;

    meta_xml_buffer.append(reinterpret_cast<const char*>(data), len);

    if (meta_xml_read_pos >= kMetaXmlCompactionThreshold) {
        compact_meta_xml_buffer();
    }

    if (meta_xml_buffer.size() > meta_xml_buffer_max) {
        compact_meta_xml_buffer();
        if (meta_xml_buffer.size() > meta_xml_buffer_max) {
            const std::size_t original_size = meta_xml_buffer.size();
            const std::size_t keep_tail = std::min(original_size, kMetaXmlStartOverlapBytes);
            if (keep_tail > 0) {
                meta_xml_buffer.erase(0, original_size - keep_tail);
            } else {
                meta_xml_buffer.clear();
            }
            meta_xml_read_pos = 0;
            ++meta_xml_dropped_docs;
            meta_xml_dropped_bytes += (original_size - keep_tail);
            if (should_sample(meta_xml_dropped_docs, drop_log_interval)) {
                std::cout << "[recorder.cpp] [Drop] metadata XML buffer overflow: size="
                          << original_size << ", max=" << meta_xml_buffer_max
                          << ", kept_tail=" << keep_tail
                          << ", dropped_docs=" << meta_xml_dropped_docs
                          << ", dropped_bytes=" << meta_xml_dropped_bytes << std::endl;
            }
        }
    }

    while (running_flag) {
        const std::size_t start_pos = find_next_metadata_start(meta_xml_buffer, meta_xml_read_pos);
        if (start_pos == std::string::npos) {
            if (meta_xml_buffer.size() > kMetaXmlStartOverlapBytes) {
                meta_xml_read_pos = meta_xml_buffer.size() - kMetaXmlStartOverlapBytes;
            } else {
                meta_xml_read_pos = 0;
            }
            break;
        }

        if (start_pos > meta_xml_read_pos) {
            meta_xml_dropped_bytes += (start_pos - meta_xml_read_pos);
        }
        meta_xml_read_pos = start_pos;

        std::size_t end_pos = 0;
        std::size_t end_len = 0;
        if (!find_next_metadata_end(meta_xml_buffer, start_pos, end_pos, end_len)) {
            const std::size_t pending_size = meta_xml_buffer.size() - start_pos;
            if (pending_size > meta_xml_doc_max_bytes) {
                ++meta_xml_dropped_docs;
                meta_xml_dropped_bytes += pending_size;
                if (should_sample(meta_xml_dropped_docs, drop_log_interval)) {
                    std::cout << "[recorder.cpp] [Drop] metadata XML pending doc exceeded max: pending="
                              << pending_size << ", doc_max=" << meta_xml_doc_max_bytes
                              << ", dropped_docs=" << meta_xml_dropped_docs
                              << ", dropped_bytes=" << meta_xml_dropped_bytes << std::endl;
                }
                meta_xml_read_pos = start_pos + 1;
                continue;
            }
            break;
        }

        const std::size_t doc_end = end_pos + end_len;
        const std::size_t doc_size = doc_end - start_pos;
        if (doc_size > meta_xml_doc_max_bytes) {
            ++meta_xml_dropped_docs;
            meta_xml_dropped_bytes += doc_size;
            if (should_sample(meta_xml_dropped_docs, drop_log_interval)) {
                std::cout << "[recorder.cpp] [Drop] metadata XML doc exceeded max: size="
                          << doc_size << ", doc_max=" << meta_xml_doc_max_bytes
                          << ", dropped_docs=" << meta_xml_dropped_docs
                          << ", dropped_bytes=" << meta_xml_dropped_bytes << std::endl;
            }
            meta_xml_read_pos = doc_end;
            if (meta_xml_read_pos >= kMetaXmlCompactionThreshold) {
                compact_meta_xml_buffer();
            }
            continue;
        }

        std::string xml_doc(meta_xml_buffer.data() + start_pos, doc_size);
        analytics.publishRaw(xml_doc);
        ++meta_xml_extracted_docs;
        meta_xml_read_pos = doc_end;
        if (meta_xml_read_pos >= kMetaXmlCompactionThreshold) {
            compact_meta_xml_buffer();
        }
    }

    if (meta_xml_read_pos >= kMetaXmlCompactionThreshold) {
        compact_meta_xml_buffer();
    }
}

bool RTSPRecorder::open_output_file(AVCodecParameters* par) {
    current_filename = std::string(VIDEO_SAVE_DIR) + "/rec_" + get_time_str() + ".mp4";
    if (avformat_alloc_output_context2(&output_ctx, nullptr, "mp4", current_filename.c_str()) < 0) return false;

    AVStream* out = avformat_new_stream(output_ctx, nullptr);
    if (!out) {
        avformat_free_context(output_ctx);
        output_ctx = nullptr;
        return false;
    }

    avcodec_parameters_copy(out->codecpar, par);
    out->codecpar->codec_tag = 0;

    if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&output_ctx->pb, current_filename.c_str(), AVIO_FLAG_WRITE) < 0) {
            avformat_free_context(output_ctx);
            output_ctx = nullptr;
            return false;
        }
    }
    if (avformat_write_header(output_ctx, nullptr) < 0) {
        if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&output_ctx->pb);
        avformat_free_context(output_ctx);
        output_ctx = nullptr;
        return false;
    }

    //std::cout << "[recorder.cpp] " << "[Rec] Start: " << current_filename << std::endl;
    start_time = std::time(nullptr);
    
    // 새 파일 시작 시 타임스탬프 상태 초기화
    last_dts = AV_NOPTS_VALUE;
    start_dts_offset = AV_NOPTS_VALUE;
    is_first_packet = true;
    
    return true;
}

void RTSPRecorder::close_current_file() {
    if (output_ctx) {
        av_write_trailer(output_ctx);
        if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&output_ctx->pb);
        avformat_free_context(output_ctx);
        output_ctx = nullptr;
        logger.enqueueRecording(current_filename);
        //std::cout << "[recorder.cpp] " << "[Rec] Saved: " << current_filename << std::endl;
    }
}

bool RTSPRecorder::connect_and_record() {
    const std::string rtsp_url = load_env_string("SFEPS_RTSP_URL", kDefaultRtspUrl);
    const RecorderRuntimeLimits limits = load_recorder_runtime_limits();
    reset_meta_xml_reassembly();

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout", "5000000", 0); 
    
    input_ctx = avformat_alloc_context();
    if (!input_ctx) {
        std::cerr << "[Error] Failed to allocate ffmpeg format context." << std::endl;
        return false;
    }
    input_ctx->interrupt_callback.callback = ffmpeg_interrupt_cb;
    input_ctx->interrupt_callback.opaque = &running_flag;
    // std::cout << "[recorder.cpp] " << "[System] Connecting to " << rtsp_url << " ..." << std::endl;
    
    if (avformat_open_input(&input_ctx, rtsp_url.c_str(), nullptr, &opts) != 0) {
        av_dict_free(&opts);
        // std::cerr << "[Error] Failed to connect! Check IP, Port(8332), or Cert." << std::endl;
        if (input_ctx) {
            avformat_free_context(input_ctx);
            input_ctx = nullptr;
        }
        return false;
    }
    av_dict_free(&opts);
    
    if (avformat_find_stream_info(input_ctx, nullptr) < 0) {
        avformat_close_input(&input_ctx);
        return false;
    }

    video_stream_idx = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    for(unsigned i=0; i<input_ctx->nb_streams; i++) 
        if(input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_DATA) meta_stream_idx = i;

    if (video_stream_idx < 0) {
        avformat_close_input(&input_ctx);
        return false;
    }
    
    std::cout << "[recorder.cpp] " << "[System] Connected! Video Stream Index: " << video_stream_idx << std::endl;

    if (!open_output_file(input_ctx->streams[video_stream_idx]->codecpar)) return false;

    AVPacket pkt;
    av_init_packet(&pkt);
    std::size_t bad_meta_streak = 0;
    std::uint64_t dropped_meta_packets = 0;
    bool force_reconnect = false;

    while (running_flag) {
        if (av_read_frame(input_ctx, &pkt) < 0) break;

        if (pkt.stream_index == video_stream_idx) {
            // 파일 분할 (키프레임 기준)
            if (std::time(nullptr) - start_time >= kSegmentDurationSec &&
                (pkt.flags & AV_PKT_FLAG_KEY)) {
                close_current_file();
                if (!open_output_file(input_ctx->streams[video_stream_idx]->codecpar)) break;
            }

            if (output_ctx) {
                // 타임스탬프 0초 리셋 로직 (Offsetting)
                if (is_first_packet) {
                    if (pkt.dts != AV_NOPTS_VALUE) {
                        start_dts_offset = pkt.dts;
                        is_first_packet = false;
                    }
                }

                if (start_dts_offset != AV_NOPTS_VALUE) {
                    if (pkt.dts != AV_NOPTS_VALUE) pkt.dts -= start_dts_offset;
                    if (pkt.pts != AV_NOPTS_VALUE) pkt.pts -= start_dts_offset;
                }

                av_packet_rescale_ts(&pkt, input_ctx->streams[video_stream_idx]->time_base, output_ctx->streams[0]->time_base);
                pkt.stream_index = 0;

                // DTS 보정
                if (last_dts != AV_NOPTS_VALUE && pkt.dts <= last_dts) {
                    int64_t diff = last_dts + 1 - pkt.dts;
                    pkt.dts += diff;
                    if (pkt.pts != AV_NOPTS_VALUE) pkt.pts += diff;
                }
                last_dts = pkt.dts;
                if (pkt.pts != AV_NOPTS_VALUE && pkt.pts < pkt.dts) pkt.pts = pkt.dts;

                av_interleaved_write_frame(output_ctx, &pkt);
            }
        } else if (pkt.stream_index == meta_stream_idx) {
            bool valid_meta = true;
            if (pkt.data == nullptr || pkt.size <= 0 ||
                static_cast<std::size_t>(pkt.size) > limits.max_meta_packet_bytes) {
                valid_meta = false;
                ++bad_meta_streak;
                std::uint64_t dropped = ++dropped_meta_packets;
                if (should_sample(dropped, limits.drop_log_interval)) {
                    std::cout << "[recorder.cpp] " << "[Drop] metadata packet rejected: size=" << pkt.size
                              << ", max=" << limits.max_meta_packet_bytes
                              << ", bad_streak=" << bad_meta_streak
                              << ", dropped_count=" << dropped << std::endl;
                }
                if (bad_meta_streak >= limits.bad_meta_streak_limit) {
                    std::cerr << "[recorder.cpp] " << "[Security] metadata bad streak reached limit ("
                              << limits.bad_meta_streak_limit
                              << "), reconnecting RTSP session." << std::endl;
                    force_reconnect = true;
                }
            }

            if (valid_meta) {
                bad_meta_streak = 0;
                process_meta_xml_chunk(reinterpret_cast<const std::uint8_t*>(pkt.data),
                                       static_cast<std::size_t>(pkt.size),
                                       limits.meta_xml_buffer_max,
                                       limits.meta_xml_doc_max_bytes,
                                       limits.drop_log_interval);
            }
        }
        av_packet_unref(&pkt);
        if (force_reconnect) break;
    }
    close_current_file();
    return true;
}

void RTSPRecorder::run() {
    while (running_flag) {
        if (!connect_and_record()) std::cerr << "[System] Connection Retry in 5s..." << std::endl;
        cleanup();
        auto waited = std::chrono::milliseconds(0);
        constexpr auto kRetrySleep = std::chrono::seconds(5);
        constexpr auto kRetrySleepStep = std::chrono::milliseconds(200);
        while (running_flag && waited < kRetrySleep) {
            const auto remain =
                std::chrono::duration_cast<std::chrono::milliseconds>(kRetrySleep - waited);
            const auto chunk = (remain < kRetrySleepStep) ? remain : kRetrySleepStep;
            std::this_thread::sleep_for(chunk);
            waited += chunk;
        }
    }
}
