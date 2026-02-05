#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <iostream>
#include <string>
#include <filesystem> // 파일 관리용 (C++17)
#include <chrono>     // 시간 계산용
#include "log.h"
#include <thread>
#include <arpa/inet.h>

// ▼▼▼ 카메라 주소 수정 필수 ▼▼▼
#define RTSP_URL "rtsp://127.0.0.1:8554/cam1" 

namespace fs = std::filesystem;

struct CustomData {
    GstElement *pipeline;
    GMainLoop *loop;
    DBLogger *logger;
};

// [추가] 로그인만 담당하는 전용 함수
void run_login_auth() {
    DBLogger db("test_db");
    if (!db.connect()) return;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(5555); // Qt 클라이언트와 약속한 포트

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5);

    while (true) {
        int client_fd = accept(server_fd, NULL, NULL);
        char buffer[1024] = {0};
        int len = read(client_fd, buffer, 1024);

        if (len > 0) {
            std::string data(buffer);
            // "ID:PW" 형식에서 ID 추출 및 검증
            size_t sep = data.find(':');
            if (sep != std::string::npos) {
                std::string user = data.substr(0, sep);
                // 일단 들어오면 무조건 PASS로 보낸 뒤 DB에 기록 (검증 로직은 필요시 추가)
                send(client_fd, "PASS", 4, 0);
                db.enqueue("LOGIN", user);
            } else {
                send(client_fd, "FAIL", 4, 0);
            }
        }
        close(client_fd);
    }
}

// 팀원이 접속했을 때 실행되는 함수 (로그 기록)
static void client_connected(GstRTSPServer *server, GstRTSPClient *client, ServerData *data) {
    std::cout << ">> New Client Connected!" << std::endl;
    // DB에 누가 들어왔다고 기록 (IP 정보 등은 심화 과정이라 생략하고 접속 사실만 기록)
    data->logger->enqueue("INFO", "Client Connected to RTSP Server");
}

int main(int argc, char *argv[]) {
    // ---------------------------------------------------------
    // 1. DB 연결 및 초기화
    // ---------------------------------------------------------
    DBLogger myLogger("CCgbd");
    if (!myLogger.connect()) {
        std::cerr << "[CRITICAL] DB Connection Failed! Server stops." << std::endl;
        return -1;
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

    std::thread auth_thread(run_login_auth);
    auth_thread.detach(); // 백그라운드에서 알아서 돌아가게 분리

    // ---------------------------------------------------------
    // 2. GStreamer RTSP 서버 설정
    // ---------------------------------------------------------
    GMainLoop *loop;
    GstRTSPServer *server;
    GstRTSPMountPoints *mounts;
    GstRTSPMediaFactory *factory;

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