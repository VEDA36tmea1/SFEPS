#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <iostream>
#include <string>
#include <filesystem> 
#include <chrono>     
#include <ctime>      
#include "log.h"

// ▼▼▼ 경로 및 주소 설정 ▼▼▼
#define VIDEO_SAVE_DIR "/home/iam/finalProject/SFEPS/videos"
#define RTSP_URL "rtsp://127.0.0.1:8554/cam1"

namespace fs = std::filesystem;

struct CustomData {
    GstElement *pipeline;
    GMainLoop *loop;
    DBLogger *logger;
};

// [기능 1] 5분 지난 파일 삭제
static gboolean cleanup_old_files(gpointer user_data) {
    const auto retention_period = std::chrono::minutes(5);
    try {
        if (!fs::exists(VIDEO_SAVE_DIR)) return TRUE;
        auto now = fs::file_time_type::clock::now();
        for (const auto& entry : fs::directory_iterator(VIDEO_SAVE_DIR)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (filename.find("rec_") == 0 && filename.find(".mp4") != std::string::npos) {
                    auto ftime = fs::last_write_time(entry);
                    if (now - ftime > retention_period) {
                        std::cout << "[Cleanup] Deleting: " << filename << std::endl;
                        fs::remove(entry.path());
                    } 
                }
            }
        }
    } catch (const std::exception& e) { std::cerr << "[Cleanup Error] " << e.what() << std::endl; }
    return TRUE; 
}

// [기능 2] DB 용량 청소
static gboolean cleanup_db_task(gpointer user_data) {
    CustomData *data = (CustomData *)user_data;
    data->logger->requestDbCleanup();
    return TRUE;
}

// [기능 3] 재연결 시도
static gboolean retry_connection(gpointer user_data) {
    CustomData *data = (CustomData *)user_data;
    std::cout << "[System] Retrying connection..." << std::endl;
    data->logger->enqueue("SYSTEM", "Retrying connection...");
    gst_element_set_state(data->pipeline, GST_STATE_PLAYING);
    return FALSE;
}

// [기능 4] 파이프라인 감시자
static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer user_data) {
    CustomData *data = (CustomData *) user_data;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ELEMENT: {
            const GstStructure *s = gst_message_get_structure(msg);
            if (gst_structure_has_name(s, "splitmuxsink-fragment-closed")) {
                const gchar *filepath = gst_structure_get_string(s, "location");
                if (filepath) {
                    auto now = std::chrono::system_clock::now();
                    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
                    char timeStr[50];
                    std::strftime(timeStr, sizeof(timeStr), "%H:%M:%S", std::localtime(&now_time));
                    std::cout << "[Recording] Saved: " << filepath << " (" << timeStr << ")" << std::endl;
                    data->logger->enqueueRecording(filepath);
                }
            }
            break;
        }
        case GST_MESSAGE_EOS:
            std::cout << "End of stream. Retrying..." << std::endl;
            gst_element_set_state(data->pipeline, GST_STATE_NULL);
            g_timeout_add_seconds(5, retry_connection, data);
            break;
        case GST_MESSAGE_ERROR: {
            gchar *debug;
            GError *error;
            gst_message_parse_error(msg, &error, &debug);
            std::cerr << "[Error] " << error->message << std::endl;
            data->logger->enqueue("ERROR", error->message);
            g_error_free(error);
            g_free(debug);
            gst_element_set_state(data->pipeline, GST_STATE_NULL);
            g_timeout_add_seconds(5, retry_connection, data);
            break;
        }
        default: break;
    }
    return TRUE;
}

// [기능 5] XML 데이터 처리
GstFlowReturn on_new_meta_data(GstElement *sink, CustomData *data) {
    GstSample *sample;
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (sample) {
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            std::string xmlString((char*)map.data, map.size);
            data->logger->parseAndLogXML(xmlString.c_str());
            gst_buffer_unmap(buffer, &map);
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    return GST_FLOW_ERROR;
}

// [기능 6] 스트림 연결 (지터 버퍼 제거 -> 직결)
static void on_pad_added(GstElement *element, GstPad *pad, gpointer user_data) {
    CustomData *data = (CustomData *)user_data;
    GstCaps *caps = gst_pad_get_current_caps(pad);
    GstStructure *str = gst_caps_get_structure(caps, 0);
    const gchar *name = gst_structure_get_name(str);
    const gchar *media = gst_structure_get_string(str, "media");

    std::cout << "[DEBUG] Pad added: Name=" << name << ", Media=" << (media ? media : "null") << std::endl;

    // 영상 -> Video Queue로 연결 (버퍼링)
    if (g_str_has_prefix(name, "video/") || (media && g_str_equal(media, "video"))) {
        std::cout << "[Video] Stream detected! Connecting to Queue..." << std::endl;
        GstElement *queue = gst_bin_get_by_name(GST_BIN(data->pipeline), "video_queue");
        if (!gst_pad_is_linked(gst_element_get_static_pad(queue, "sink"))) {
            gst_element_link_pads(element, gst_pad_get_name(pad), queue, "sink");
        }
    }
    // 데이터 -> Meta Queue로 연결
    else if (g_str_has_prefix(name, "application") && (media && g_str_equal(media, "application"))) {
        std::cout << "[Metadata] Stream detected! Connecting to Queue..." << std::endl;
        GstElement *queue = gst_bin_get_by_name(GST_BIN(data->pipeline), "meta_queue");
        if (!gst_pad_is_linked(gst_element_get_static_pad(queue, "sink"))) {
             gst_element_link_pads(element, gst_pad_get_name(pad), queue, "sink");
        }
    }
    gst_caps_unref(caps);
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);

    if (!fs::exists(VIDEO_SAVE_DIR)) {
        fs::create_directories(VIDEO_SAVE_DIR);
        fs::permissions(VIDEO_SAVE_DIR, fs::perms::owner_all | fs::perms::group_all | fs::perms::others_all, fs::perm_options::add);
    }

    DBLogger myLogger;
    if (!myLogger.connect()) return -1;
    myLogger.enqueue("SYSTEM", "Server Started (Max Buffer Mode)");

    CustomData data;
    data.loop = g_main_loop_new(NULL, FALSE);
    data.logger = &myLogger;

    data.pipeline = gst_pipeline_new("sfeps-pipeline");
    GstElement *source = gst_element_factory_make("rtspsrc", "source");
    
    // 단순 큐 사용 (지터 버퍼 제거)
    GstElement *v_queue = gst_element_factory_make("queue", "video_queue");
    GstElement *m_queue = gst_element_factory_make("queue", "meta_queue");

    GstElement *v_depay = gst_element_factory_make("rtph264depay", "video_depay");
    GstElement *v_parse = gst_element_factory_make("h264parse", "video_parse");
    GstElement *v_rec = gst_element_factory_make("splitmuxsink", "video_rec");
    GstElement *appsink = gst_element_factory_make("appsink", "meta_sink");

    if (!data.pipeline || !source || !v_queue || !m_queue || !v_depay || !v_parse || !v_rec || !appsink) {
        g_printerr("Elements creation failed.\n");
        return -1;
    }

    gst_bin_add_many(GST_BIN(data.pipeline), source, v_queue, m_queue, v_depay, v_parse, v_rec, appsink, NULL);
    
    // 연결: Source(Pad) -> Queue -> Depay -> Parse -> Rec
    if (!gst_element_link_many(v_queue, v_depay, v_parse, v_rec, NULL)) {
        g_printerr("Video elements link failed.\n");
        return -1;
    }
    if (!gst_element_link_many(m_queue, appsink, NULL)) {
        g_printerr("Meta elements link failed.\n");
        return -1;
    }

    // ★★★ [최종 해결책] 5초 지연 허용 + 패킷 절대 안 버림 ★★★
    g_object_set(source, 
        "location", RTSP_URL, 
        "latency", 5000,            // ★ 5초 버퍼 (3.3초 렉 커버)
        "protocols", 4,             // TCP 강제
        "use-pipeline-clock", TRUE, // 카메라 시간 무시 (시스템 시간 기준)
        "do-retransmission", FALSE, 
        "drop-on-latency", FALSE,   // ★★★ 절대 버리지 않음! (이게 핵심)
        "timeout", 30000000,        // 30초 타임아웃
        NULL);

    // 큐 용량 넉넉하게
    g_object_set(v_queue, "max-size-time", 10000000000ULL, NULL); // 10초

    g_object_set(v_parse, "config-interval", -1, NULL);

    std::string file_pattern = std::string(VIDEO_SAVE_DIR) + "/rec_%04d.mp4";
    g_object_set(v_rec, 
        "location", file_pattern.c_str(), 
        "max-size-time", 60000000000ULL, // 1분
        "send-keyframe-requests", FALSE, // 카메라 간섭 금지
        "async-handling", TRUE,
        NULL);

    g_object_set(appsink, "emit-signals", TRUE, "sync", FALSE, NULL);
    
    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_meta_data), &data);
    g_signal_connect(source, "pad-added", G_CALLBACK(on_pad_added), &data);
    
    g_timeout_add_seconds(60, cleanup_old_files, NULL);
    g_timeout_add_seconds(60, cleanup_db_task, &data);

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(data.pipeline));
    gst_bus_add_watch(bus, bus_call, &data);
    gst_object_unref(bus);

    std::cout << "Server Running: Max Buffer Mode (5s Latency, No Drop)" << std::endl;
    gst_element_set_state(data.pipeline, GST_STATE_PLAYING);
    g_main_loop_run(data.loop);

    gst_element_set_state(data.pipeline, GST_STATE_NULL);
    gst_object_unref(data.pipeline);
    g_main_loop_unref(data.loop);
    return 0;
}