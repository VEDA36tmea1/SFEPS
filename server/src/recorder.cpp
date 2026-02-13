#include "recorder.h"
#include <iostream>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <iomanip>

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
    avcodec_parameters_copy(out->codecpar, par);
    out->codecpar->codec_tag = 0;

    if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&output_ctx->pb, current_filename.c_str(), AVIO_FLAG_WRITE) < 0) return false;
    }
    if (avformat_write_header(output_ctx, nullptr) < 0) return false;

    std::cout << "[Rec] Start: " << current_filename << std::endl;
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
        std::cout << "[Rec] Saved: " << current_filename << std::endl;
    }
}

bool RTSPRecorder::connect_and_record() {
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout", "5000000", 0); 

    // ★ [보안 핵심] 자가 서명 인증서(Self-Signed) 허용 옵션
    // 이 줄이 없으면 "SSL certificate problem" 에러가 뜨면서 접속이 안 됩니다.
    av_dict_set(&opts, "tls_verify", "0", 0); 
    
    std::cout << "[System] Connecting to " << RTSP_URL << " (Secure Mode)..." << std::endl;
    
    if (avformat_open_input(&input_ctx, RTSP_URL, nullptr, &opts) != 0) {
        std::cerr << "[Error] Failed to connect! Check IP, Port(8332), or Cert." << std::endl;
        return false;
    }
    
    if (avformat_find_stream_info(input_ctx, nullptr) < 0) return false;

    video_stream_idx = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    for(unsigned i=0; i<input_ctx->nb_streams; i++) 
        if(input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_DATA) meta_stream_idx = i;

    if (video_stream_idx < 0) return false;
    
    // 연결 성공 로그
    logger.enqueue("SYSTEM", "RTSP Connected via TLS (Secure)");
    std::cout << "[System] Connected! Video Stream Index: " << video_stream_idx << std::endl;

    if (!open_output_file(input_ctx->streams[video_stream_idx]->codecpar)) return false;

    AVPacket pkt;
    av_init_packet(&pkt);

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
            std::string xml((char*)pkt.data, pkt.size);
            // publish raw metadata to analytics processor (may contain multiple lines)
            analytics.publishRaw(xml);
        }
        av_packet_unref(&pkt);
    }
    close_current_file();
    return true;
}

void RTSPRecorder::run() {
    while (running_flag) {
        if (!connect_and_record()) std::cerr << "[System] Connection Retry in 5s..." << std::endl;
        cleanup();
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}