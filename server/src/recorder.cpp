#include "recorder.h"
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <iostream>
#include <limits>
#include <unistd.h>

namespace {
std::size_t load_env_size_t(const char* name, std::size_t default_value, std::size_t min_value) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') return default_value;

    errno = 0;
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (errno != 0 || end == raw || (end != nullptr && *end != '\0') || parsed < min_value ||
        parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        std::cerr << "[recorder.cpp] " << "Invalid env " << name << "=" << raw
                  << ", using default=" << default_value << std::endl;
        return default_value;
    }
    return static_cast<std::size_t>(parsed);
}

bool should_sample(std::uint64_t counter, std::size_t interval) {
    if (counter == 1) return true;
    if (interval == 0) return false;
    return (counter % interval) == 0;
}

std::string derive_verify_host_from_url(const std::string& url) {
    const std::size_t scheme_pos = url.find("://");
    const std::size_t host_start = (scheme_pos == std::string::npos) ? 0 : scheme_pos + 3;
    if (host_start >= url.size()) return std::string();

    if (url[host_start] == '[') {
        const std::size_t host_end = url.find(']', host_start + 1);
        if (host_end == std::string::npos || host_end <= host_start + 1) return std::string();
        return url.substr(host_start + 1, host_end - host_start - 1);
    }

    const std::size_t host_end = url.find_first_of(":/", host_start);
    if (host_end == std::string::npos) {
        return url.substr(host_start);
    }
    if (host_end <= host_start) return std::string();
    return url.substr(host_start, host_end - host_start);
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
    const std::size_t max_meta_packet_bytes = load_env_size_t("SFEPS_META_MAX_PACKET_BYTES", 65536, 1);
    const std::size_t bad_meta_streak_limit = load_env_size_t("SFEPS_META_BAD_STREAK_LIMIT", 20, 1);
    const std::size_t drop_log_interval = load_env_size_t("SFEPS_DROP_LOG_INTERVAL", 100, 1);

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout", "5000000", 0); 

    const char* tls_ca = std::getenv("RTSPS_TLS_CA");
    if (tls_ca == nullptr || tls_ca[0] == '\0') {
        std::cerr << "[Error] RTSPS_TLS_CA is not set (fail-closed)." << std::endl;
        av_dict_free(&opts);
        return false;
    }
    if (access(tls_ca, R_OK) != 0) {
        std::cerr << "[Error] RTSPS_TLS_CA is not readable: " << tls_ca << std::endl;
        av_dict_free(&opts);
        return false;
    }

    const char* verify_host_env = std::getenv("SFEPS_RTSPS_VERIFYHOST");
    std::string verify_host;
    if (verify_host_env != nullptr && verify_host_env[0] != '\0') {
        verify_host = verify_host_env;
    } else {
        verify_host = derive_verify_host_from_url(RTSP_URL);
    }
    if (verify_host.empty()) {
        std::cerr << "[Error] Unable to resolve TLS verify host from RTSP_URL." << std::endl;
        av_dict_free(&opts);
        return false;
    }

    av_dict_set(&opts, "tls_verify", "1", 0);
    av_dict_set(&opts, "ca_file", tls_ca, 0);
    av_dict_set(&opts, "verifyhost", verify_host.c_str(), 0);
    
    input_ctx = avformat_alloc_context();
    if (!input_ctx) {
        std::cerr << "[Error] Failed to allocate ffmpeg format context." << std::endl;
        return false;
    }
    input_ctx->interrupt_callback.callback = ffmpeg_interrupt_cb;
    input_ctx->interrupt_callback.opaque = &running_flag;
    std::cout << "[recorder.cpp] " << "[System] Connecting to " << RTSP_URL << " (Secure Mode)..." << std::endl;
    
    if (avformat_open_input(&input_ctx, RTSP_URL, nullptr, &opts) != 0) {
        av_dict_free(&opts);
        std::cerr << "[Error] Failed to connect! Check IP, Port(8332), or Cert." << std::endl;
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
            if (std::time(nullptr) - start_time >= SEGMENT_DURATION && (pkt.flags & AV_PKT_FLAG_KEY)) {
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
            if (pkt.data == nullptr || pkt.size <= 0 || static_cast<std::size_t>(pkt.size) > max_meta_packet_bytes) {
                valid_meta = false;
                ++bad_meta_streak;
                std::uint64_t dropped = ++dropped_meta_packets;
                if (should_sample(dropped, drop_log_interval)) {
                    std::cout << "[recorder.cpp] " << "[Drop] metadata packet rejected: size=" << pkt.size
                              << ", max=" << max_meta_packet_bytes
                              << ", bad_streak=" << bad_meta_streak
                              << ", dropped_count=" << dropped << std::endl;
                }
                if (bad_meta_streak >= bad_meta_streak_limit) {
                    std::cerr << "[recorder.cpp] " << "[Security] metadata bad streak reached limit ("
                              << bad_meta_streak_limit << "), reconnecting RTSP session." << std::endl;
                    force_reconnect = true;
                }
            }

            if (valid_meta) {
                bad_meta_streak = 0;
                std::string xml(reinterpret_cast<char*>(pkt.data), pkt.size);
                // publish raw metadata to analytics processor (may contain multiple lines)
                analytics.publishRaw(xml);
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
