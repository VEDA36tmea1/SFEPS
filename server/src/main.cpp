#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <iostream>
#include <string>
#include <filesystem> // 파일 관리용 (C++17)
#include <chrono>     // 시간 계산용
#include "log.h"

// ▼▼▼ 카메라 주소 수정 필수 ▼▼▼
#define RTSP_URL "rtsp://127.0.0.1:8554/cam1" 
#define video_dir "/home/iam/finalProject/SFEPS/videos"

namespace fs = std::filesystem;

struct CustomData {
    GstElement *pipeline;
    GMainLoop *loop;
    DBLogger *logger;
};

// [신규 기능] 5분 지난 녹화 파일 자동 삭제 청소부
static gboolean cleanup_old_files(gpointer user_data) {
    // 보관 기간: 5분
    const auto retention_period = std::chrono::minutes(5);
    // [수정] 청소할 폴더 경로 지정
    
    try {
        // 현재 시간 (파일 시스템 시계 기준)
        auto now = fs::file_time_type::clock::now();

        // 현재 폴더(".") 내의 모든 파일을 검사
        for (const auto& entry : fs::directory_iterator(video_dir)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                
                // 우리가 만든 녹화 파일인지 확인 (rec_로 시작하고 .mp4로 끝남)
                if (filename.find("rec_") == 0 && filename.find(".mp4") != std::string::npos) {
                    
                    // 파일의 마지막 수정 시간 확인
                    auto ftime = fs::last_write_time(entry);
                    
                    // 파일 나이 계산 (분 단위)
                    auto age = std::chrono::duration_cast<std::chrono::minutes>(now - ftime).count();
                    
                    // (현재시간 - 수정시간)이 5분보다 크면 삭제
                    if (now - ftime > retention_period) {
                        std::cout << "  [DELETE] " << filename << " (Age: " << age << "m > 5m)" << std::endl;
                        fs::remove(entry.path());
                    } else {
                        // 삭제 안 된 이유 출력 (너무 많으면 주석 처리)
                        // std::cout << "  [KEEP]   " << filename << " (Age: " << age << "m)" << std::endl;
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[Cleanup Error] " << e.what() << std::endl;
    }
    
    return TRUE; // TRUE를 반환해야 타이머가 꺼지지 않고 계속 반복됩니다.
}


// [신규 기능] DB 용량 관리 청소부 (1분마다 실행)
static gboolean cleanup_db_task(gpointer user_data) {
    CustomData *data = (CustomData *)user_data;
    // DBLogger 스레드에게 "용량 확인해봐"라고 요청
    data->logger->requestDbCleanup();
    return TRUE;
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
/*
// ★★★ [테스트 함수] 가짜 XML 데이터 생성 및 주입 ★★★
// 나중에 이 함수 전체를 지우거나 주석 처리하면 됩니다.
static gboolean test_fake_xml_injection(gpointer user_data) {
    CustomData *data = (CustomData *)user_data;

    // 현재 시간 구하기
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    char timeBuffer[30];
    std::strftime(timeBuffer, sizeof(timeBuffer), "%Y-%m-%dT%H:%M:%S.000Z", std::gmtime(&now_time));

    // 가짜 XML 데이터 (사람 감지됨)
    std::string fakeXML = R"(
        <tt:MetadataStream>
          <tt:VideoAnalytics>
            <tt:Frame UtcTime=")" + std::string(timeBuffer) + R"(">
              <tt:Object ObjectId="12345">
                <tt:Appearance>
                  <tt:Class>
                    <tt:Type Likelihood="0.99">Human</tt:Type>
                  </tt:Class>
                  <tt:HumanBody>
                     <bd:Gender>Male</bd:Gender>
                     <bd:Clothing>
                        <bd:Tops><tt:ColorString>Red</tt:ColorString></bd:Tops>
                        <bd:Bottoms><tt:ColorString>Black</tt:ColorString></bd:Bottoms>
                     </bd:Clothing>
                  </tt:HumanBody>
                </tt:Appearance>
              </tt:Object>
            </tt:Frame>
          </tt:VideoAnalytics>
        </tt:MetadataStream>
    )";

    std::cout << "[TEST] Injecting Fake XML Data (Human Detected)..." << std::endl;
    
    // DB 로거에 주입 (카메라에서 온 것처럼 위장)
    data->logger->parseAndLogXML(fakeXML.c_str());

    return TRUE; // 10초마다 계속 반복
}
*/
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
        default: break;
    }
    return TRUE;
}

// [기능 5] XML 데이터 수신 및 DB 저장
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

// [기능 6] 스트림 연결
static void on_pad_added(GstElement *element, GstPad *pad, gpointer user_data) {
    CustomData *data = (CustomData *)user_data;
    GstCaps *caps = gst_pad_get_current_caps(pad);
    GstStructure *str = gst_caps_get_structure(caps, 0);

    // 1. 캡슐의 이름(name)과 미디어 타입(media)을 모두 확인
    const gchar *name = gst_structure_get_name(str);
    const gchar *media = gst_structure_get_string(str, "media");
    const gchar *encoding = gst_structure_get_string(str, "encoding-name");

    std::cout << "[DEBUG] Pad added: Name=" << name 
              << ", Media=" << (media ? media : "null") 
              << ", Encoding=" << (encoding ? encoding : "null") << std::endl;

   // 영상 스트림 -> 바로 depay로 연결
    if (g_str_has_prefix(name, "video/") || (media && g_str_equal(media, "video"))) {
        std::cout << "[Video] Stream detected! Connecting to Recorder..." << std::endl;
        
        // 지터 버퍼 없이 바로 depay로!
        GstElement *depay = gst_bin_get_by_name(GST_BIN(data->pipeline), "video_depay");
        if (!gst_pad_is_linked(gst_element_get_static_pad(depay, "sink"))) {
            gst_element_link_pads(element, gst_pad_get_name(pad), depay, "sink");
        }
    }

    // 3. 메타데이터 스트림 처리 (media가 "application" 이고 encoding이 없는 경우 등)
    // 주의: 영상(RTP)도 application/x-rtp라서 헷갈릴 수 있음. media="application"을 꼭 확인.
    else if (g_str_has_prefix(name, "application") && (media && g_str_equal(media, "application"))) {
        std::cout << "[Metadata] Stream detected! Connecting to DB..." << std::endl;
        
        GstElement *appsink = gst_bin_get_by_name(GST_BIN(data->pipeline), "meta_sink");
        if (!gst_pad_is_linked(gst_element_get_static_pad(appsink, "sink"))) {
             gst_element_link_pads(element, gst_pad_get_name(pad), appsink, "sink");
        }
    }
    gst_caps_unref(caps);
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);

    DBLogger myLogger;
    if (!myLogger.connect()) return -1;
    myLogger.enqueue("SYSTEM", "smart_server Started");

    CustomData data;
    data.loop = g_main_loop_new(NULL, FALSE);
    data.logger = &myLogger;

    data.pipeline = gst_pipeline_new("CCgbd-pipeline");
    GstElement *source = gst_element_factory_make("rtspsrc", "source");
    
    // [수정 2] 녹화 안정화를 위한 지터 버퍼 추가
    //GstElement *v_jitter = gst_element_factory_make("rtpjitterbuffer", "video_jitter");

    GstElement *v_depay = gst_element_factory_make("rtph264depay", "video_depay");
    GstElement *v_parse = gst_element_factory_make("h264parse", "video_parse");
    GstElement *v_rec = gst_element_factory_make("splitmuxsink", "video_rec");
    GstElement *appsink = gst_element_factory_make("appsink", "meta_sink");

    if (!data.pipeline || !source || !v_depay || !v_parse || !v_rec || !appsink) {
        g_printerr("Elements creation failed.\n");
        return -1;
    }

    gst_bin_add_many(GST_BIN(data.pipeline), source, v_depay, v_parse, v_rec, appsink, NULL);
    
    // 연결 순서: jitter -> depay -> parse -> rec
    gst_element_link_many( v_depay, v_parse, v_rec, NULL);

    // latency 2초로 설정하여 끊김 방지
    // DB 데이터는 XML 내용 그대로 저장되므로 영향 없음! 영상 끊김만 해결해 줌.
    g_object_set(source, 
        "location", RTSP_URL, 
        "latency", 2000, 
        "protocols", 4, 
        "use-pipeline-clock", TRUE,
        "do-retransmission", FALSE, // TCP라 재전송 불필요
        "drop-on-latency", TRUE,    // 늦은 패킷은 버림 (시간 밀림 방지)
        "timeout", 30000000, 
    
        NULL);
    //?
    g_object_set(v_parse, "config-interval", -1, NULL);
    /*
    g_object_set(v_jitter, 
        "do-lost", TRUE, 
        "drop-on-latency", TRUE, // ★ 늦게 온 패킷은 버려서 밀림 방지
        NULL);
    */
    // [설정 2] 녹화 설정: 60초(1분)마다 자르기
    // 60초 = 60,000,000,000 나노초
    std::string file_pattern = std::string(video_dir) + "/rec_%04d.mp4";
    g_object_set(v_rec, 
        "location", file_pattern.c_str(), 
        "max-size-time", 60000000000ULL,
        "send-keyframe-requests", TRUE, // 파일 자를 때 키프레임 요청
        "async-handling", TRUE,         // 비동기 처리로 버벅임 방지
        NULL);

    g_object_set(appsink, "emit-signals", TRUE, "sync", FALSE, NULL);
    
    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_meta_data), &data);
    g_signal_connect(source, "pad-added", G_CALLBACK(on_pad_added), &data);
    
    // 청소부 함수는 60초마다 실행
    g_timeout_add_seconds(60, cleanup_old_files, NULL);



    // ★★★ [테스트] 10초마다 가짜 데이터 주입 (나중에 이 줄만 지우면 됨) ★★★
    //g_timeout_add_seconds(10, test_fake_xml_injection, &data);


    // [신규] DB 청소 (60초마다 실행 -> 100MB 넘으면 삭제)
    g_timeout_add_seconds(60, cleanup_db_task, &data);

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