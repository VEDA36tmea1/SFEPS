#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <iostream>
#include <string>
#include <filesystem> // 파일 관리용 (C++17)
#include <chrono>     // 시간 계산용
#include "log.h"

// ▼▼▼ 카메라 주소 수정 필수 ▼▼▼
#define RTSP_URL "rtsp://127.0.0.1:8554/cam1" 

namespace fs = std::filesystem;

struct CustomData {
    GstElement *pipeline;
    GMainLoop *loop;
    DBLogger *logger;
};

// [신규 기능] 1시간 지난 녹화 파일 자동 삭제 청소부
static gboolean cleanup_old_files(gpointer user_data) {
    // 보관 기간: 1시간
    const auto retention_period = std::chrono::hours(1);
    
    try {
        // 현재 시간 (파일 시스템 시계 기준)
        auto now = fs::file_time_type::clock::now();

        // 현재 폴더(".") 내의 모든 파일을 검사
        for (const auto& entry : fs::directory_iterator(".")) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                
                // 우리가 만든 녹화 파일인지 확인 (rec_로 시작하고 .mp4로 끝남)
                if (filename.find("rec_") == 0 && filename.find(".mp4") != std::string::npos) {
                    
                    // 파일의 마지막 수정 시간 확인
                    auto ftime = fs::last_write_time(entry);
                    
                    // (현재시간 - 수정시간)이 1시간보다 크면 삭제
                    if (now - ftime > retention_period) {
                        std::cout << "[Auto Cleanup] Deleting old recording: " << filename << std::endl;
                        fs::remove(entry.path());
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[Cleanup Error] " << e.what() << std::endl;
    }
    
    return TRUE; // TRUE를 반환해야 타이머가 꺼지지 않고 계속 반복됩니다.
}

// [신규] 재연결 시도 함수 (5초 뒤 실행됨)
static gboolean retry_connection(gpointer user_data) {
    CustomData *data = (CustomData *)user_data;
    
    std::cout << "[System] Retrying connection to camera..." << std::endl;
    data->logger->enqueue("SYSTEM", "Retrying connection...");

    // 다시 시작 시도 (PLAYING)
    gst_element_set_state(data->pipeline, GST_STATE_PLAYING);
    
    return FALSE; // FALSE를 리턴해야 타이머가 한 번만 실행되고 사라짐
}


// [수정됨] 파이프라인 감시자 (에러 나도 안 죽고 재시도)
static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer user_data) {
    CustomData *data = (CustomData *) user_data;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ELEMENT: {
            const GstStructure *s = gst_message_get_structure(msg);
            if (gst_structure_has_name(s, "splitmuxsink-fragment-closed")) {
                const gchar *filepath = gst_structure_get_string(s, "location");
                if (filepath) {
                    std::cout << "[Recording] File Saved: " << filepath << std::endl;
                    data->logger->enqueueRecording(filepath);
                }
            }
            break;
        }
        case GST_MESSAGE_EOS:
            std::cout << "End of stream (Camera disconnected?)" << std::endl;
            data->logger->enqueue("INFO", "Stream Ended. Retrying...");
            
            // 연결 끊기면 -> 멈추고 재시도
            gst_element_set_state(data->pipeline, GST_STATE_NULL);
            g_timeout_add_seconds(5, retry_connection, data);
            break;

        case GST_MESSAGE_ERROR: {
            gchar *debug;
            GError *error;
            gst_message_parse_error(msg, &error, &debug);
            
            std::cerr << "[Error] " << error->message << std::endl;
            // DB에 에러 기록
            data->logger->enqueue("ERROR", error->message);

            g_error_free(error);
            g_free(debug);

            // ★ 중요: 에러가 나도 프로그램을 끄지 않음 (g_main_loop_quit 제거) ★
            
            // 1. 일단 파이프라인 멈춤 (리셋 효과)
            gst_element_set_state(data->pipeline, GST_STATE_NULL);

            // 2. 5초 뒤에 재연결 시도하도록 예약
            std::cout << "[System] Waiting 5 seconds before reconnect..." << std::endl;
            g_timeout_add_seconds(5, retry_connection, data);
            break;
        }
        default:
            break;
    }
    return TRUE;
}

// [핵심 1] XML 데이터 수신 및 DB 저장
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

// [핵심 2] 스트림 연결
static void on_pad_added(GstElement *element, GstPad *pad, gpointer user_data) {
    CustomData *data = (CustomData *)user_data;
    GstCaps *caps = gst_pad_get_current_caps(pad);
    GstStructure *str = gst_caps_get_structure(caps, 0);
    const gchar *name = gst_structure_get_name(str);

    if (g_str_has_prefix(name, "video/")) {
        GstElement *depay = gst_bin_get_by_name(GST_BIN(data->pipeline), "video_depay");
        // 이미 연결되어 있으면 건너뜀 (재연결 시 중요)
        if (!gst_pad_is_linked(gst_element_get_static_pad(depay, "sink"))) {
            std::cout << "[Video] Re-linking stream..." << std::endl;
            gst_element_link_pads(element, gst_pad_get_name(pad), depay, "sink");
        }
    }
    else if (g_str_has_prefix(name, "application")) {
        GstElement *appsink = gst_bin_get_by_name(GST_BIN(data->pipeline), "meta_sink");
        if (!gst_pad_is_linked(gst_element_get_static_pad(appsink, "sink"))) {
             std::cout << "[Metadata] Re-linking stream..." << std::endl;
             gst_element_link_pads(element, gst_pad_get_name(pad), appsink, "sink");
        }
    }
    gst_caps_unref(caps);
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);

    DBLogger myLogger;
    if (!myLogger.connect()) return -1;
    myLogger.enqueue("SYSTEM", "SFEPS Server Started");

    CustomData data;
    data.loop = g_main_loop_new(NULL, FALSE);
    data.logger = &myLogger;

    data.pipeline = gst_pipeline_new("sfeps-pipeline");
    GstElement *source = gst_element_factory_make("rtspsrc", "source");
    GstElement *v_depay = gst_element_factory_make("rtph264depay", "video_depay");
    GstElement *v_parse = gst_element_factory_make("h264parse", "video_parse");
    GstElement *v_rec = gst_element_factory_make("splitmuxsink", "video_rec");
    GstElement *appsink = gst_element_factory_make("appsink", "meta_sink");

    if (!data.pipeline || !source || !v_depay || !v_parse || !v_rec || !appsink) {
        g_printerr("Elements creation failed.\n");
        return -1;
    }

    gst_bin_add_many(GST_BIN(data.pipeline), source, v_depay, v_parse, v_rec, appsink, NULL);
    gst_element_link_many(v_depay, v_parse, v_rec, NULL);

    g_object_set(source, "location", RTSP_URL, "latency", 0, NULL);
    
    // [설정 2] 녹화 설정: 60초(1분)마다 자르기
    // 60초 = 60,000,000,000 나노초
    g_object_set(v_rec, 
        "location", "rec_%04d.mp4", 
        "max-size-time", 60000000000ULL, 
        NULL);

    g_object_set(appsink, "emit-signals", TRUE, "sync", FALSE, NULL);
    
    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_meta_data), &data);
    g_signal_connect(source, "pad-added", G_CALLBACK(on_pad_added), &data);
    
    // 청소부 함수는 60초마다 실행
    g_timeout_add_seconds(60, cleanup_old_files, NULL);

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(data.pipeline));
    gst_bus_add_watch(bus, bus_call, &data);
    gst_object_unref(bus);

    std::cout << "Server Running: Recording 1min chunks, Keeping last 5 mins." << std::endl;
    gst_element_set_state(data.pipeline, GST_STATE_PLAYING);
    g_main_loop_run(data.loop);

    gst_element_set_state(data.pipeline, GST_STATE_NULL);
    gst_object_unref(data.pipeline);
    g_main_loop_unref(data.loop);
    return 0;
}