#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <iostream>
#include <string>
#include <filesystem> 
#include <chrono>     
#include <ctime>      
#include "log.h"
#include "auth.h"
#include <thread>
#include <arpa/inet.h>

// ▼▼▼ 경로 및 주소 설정 ▼▼▼
#define VIDEO_SAVE_DIR "/home/iam/finalProject/SFEPS/videos"
#define RTSP_URL "rtsp://192.168.0.92/cam1"

namespace fs = std::filesystem;

// [설정] 로그인 인증 전용 포트 및 DB 접속 정보
#define AUTH_PORT 5555           // Qt 클라이언트와 통신할 포트
#define DB_HOST "192.168.0.92"   // MariaDB 서버 IP
#define DB_USER "pi"             // DB 사용자 아이디
#define DB_PASS "raspberry"      // DB 비밀번호
#define DB_NAME "Client_db"      // 사용할 데이터베이스 이름

// 데이터를 콜백 함수로 넘기기 위한 구조체
struct ServerData {
    DBLogger *logger;
};

// 로그인 인증 전용 스레드 함수
void run_login_auth() {
    DBLogger db(DB_NAME); // 로그 기록용 객체
    Authenticator auth(DB_HOST, DB_USER, DB_PASS, DB_NAME); // ID/PW 검증용 객체
    
    // DB 연결 확인 (로그용, 인증용 각각 연결)
    if (!db.connect() || !auth.connect()) {
        std::cerr << "[Fatal] Auth-related DB connection failed." << std::endl;
        return;
    }

    // TCP 소켓 서버 설정
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 주소 및 포트 바인딩 (간결한 구조체 초기화 방식 사용)
    struct sockaddr_in addr = {AF_INET, htons(AUTH_PORT), {INADDR_ANY}};
    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5); // 최대 5개 대기열

    while (true) {
        // 클라이언트 접속 대기
        int client_fd = accept(server_fd, NULL, NULL);
        char buf[1024] = {0};

        // 데이터 수신 ("ID:PW" 형식 예상)
        if (read(client_fd, buf, sizeof(buf)) > 0) {
            std::string data(buf), user = "Unknown";
            size_t sep = data.find(':');
            bool success = false;

            // 구분자(:)가 있을 경우에만 분석 진행
            if (sep != std::string::npos) {
                user = data.substr(0, sep);                 // ID 추출
                std::string pass = data.substr(sep + 1);    // PW 추출
                success = auth.authenticate(user, pass);     // DB 조회 및 검증
            }  
              
            // 검증 결과 전송 및 로그 기록 (삼항 연산자로 간소화)
            send(client_fd, success ? "PASS" : "FAIL", 4, 0);
            db.enqueue(success ? "LOGIN_SUCCESS" : "LOGIN_FAIL", user);
        }
        close(client_fd); // 세션 종료
    }
}

struct CustomData {
    GstElement *pipeline;
    GMainLoop *loop;
    DBLogger *logger;
};

// [기능 1] 파일 삭제 (C++17 호환성 수정)
static gboolean cleanup_old_files(gpointer user_data) {
    // ★ 설정: 60초(1분) 지난 파일 삭제 (테스트용) ★
    const long retention_seconds = 60; 
    
    // std::cout << "[Auto Cleanup] Checking files..." << std::endl;

    try {
        if (!fs::exists(VIDEO_SAVE_DIR)) return TRUE;

        // C++17 방식: 파일 시스템 시계를 사용
        auto now = fs::file_time_type::clock::now();

        for (const auto& entry : fs::directory_iterator(VIDEO_SAVE_DIR)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                
                // rec_*.mp4 파일만 대상
                if (filename.find("rec_") == 0 && filename.find(".mp4") != std::string::npos) {
                    
                    auto ftime = fs::last_write_time(entry);
                    
                    // 나이 계산 (현재시간 - 파일시간) -> 초 단위 변환
                    auto diff = now - ftime;
                    auto age_seconds = std::chrono::duration_cast<std::chrono::seconds>(diff).count();
                    
                    if (age_seconds >= retention_seconds) {
                        std::cout << "  🗑️ [DELETE] " << filename << " (Age: " << age_seconds << "s)" << std::endl;
                        fs::remove(entry.path());
                    } else {
                        // 삭제 대기 중 확인 (필요시 주석 해제)
                        // std::cout << "  👁️ [KEEP]   " << filename << " (Age: " << age_seconds << "s)" << std::endl;
                    }
                }
            }
        }
    catch (const std::exception& e) {
        std::cerr << "[Cleanup Error] " << e.what() << std::endl;
    }
    
    return TRUE; 
}

// [기능 2] DB 용량 관리
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

// [기능 6] 스트림 연결
static void on_pad_added(GstElement *element, GstPad *pad, gpointer user_data) {
    CustomData *data = (CustomData *)user_data;
    GstCaps *caps = gst_pad_get_current_caps(pad);
    GstStructure *str = gst_caps_get_structure(caps, 0);
    const gchar *name = gst_structure_get_name(str);
    const gchar *media = gst_structure_get_string(str, "media");

    std::cout << "[DEBUG] Pad added: Name=" << name << ", Media=" << (media ? media : "null") << std::endl;

    if (g_str_has_prefix(name, "video/") || (media && g_str_equal(media, "video"))) {
        std::cout << "[Video] Stream detected! Connecting to Recorder..." << std::endl;
        GstElement *depay = gst_bin_get_by_name(GST_BIN(data->pipeline), "video_depay");
        if (!gst_pad_is_linked(gst_element_get_static_pad(depay, "sink"))) {
            gst_element_link_pads(element, gst_pad_get_name(pad), depay, "sink");
        }
    }
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

    if (!fs::exists(VIDEO_SAVE_DIR)) {
        fs::create_directories(VIDEO_SAVE_DIR);
        fs::permissions(VIDEO_SAVE_DIR, fs::perms::owner_all | fs::perms::group_all | fs::perms::others_all, fs::perm_options::add);
    }

    DBLogger myLogger;
    if (!myLogger.connect()) return -1;
    myLogger.enqueue("SYSTEM", "Server Started (Main Recording Mode)");

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
    
    // 연결: Depay -> Parse -> Rec
    if (!gst_element_link_many(v_depay, v_parse, v_rec, NULL)) {
        g_printerr("Video elements link failed.\n");
        return -1;
    }

    // ★ [설정] 끊김 방지 최적화 ★
    g_object_set(source, 
        "location", RTSP_URL, 
        "latency", 2000, 
        "protocols", 4,         
        "use-pipeline-clock", TRUE, 
        "do-retransmission", FALSE,
        "drop-on-latency", FALSE, 
        "timeout", 30000000,
        NULL);

    g_object_set(v_parse, "config-interval", -1, NULL);

    std::string file_pattern = std::string(VIDEO_SAVE_DIR) + "/rec_%04d.mp4";
    g_object_set(v_rec, 
        "location", file_pattern.c_str(), 
        "max-size-time", 60000000000ULL, // 1분 (60초)
        "send-keyframe-requests", FALSE, // 카메라 간섭 금지
        "async-handling", TRUE,
        NULL);

    g_object_set(appsink, "emit-signals", TRUE, "sync", FALSE, NULL);
    
    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_meta_data), &data);
    g_signal_connect(source, "pad-added", G_CALLBACK(on_pad_added), &data);
    
    // ★ [중요] 30초마다 파일 검사 -> 1분 넘은 거 삭제
    g_timeout_add_seconds(60, cleanup_old_files, NULL);
    g_timeout_add_seconds(300, cleanup_db_task, &data);

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(data.pipeline));
    gst_bus_add_watch(bus, bus_call, &data);
    gst_object_unref(bus);

    std::cout << "Server Running: C++ Recording & Auto Delete" << std::endl;
    gst_element_set_state(data.pipeline, GST_STATE_PLAYING);
    g_main_loop_run(data.loop);

    gst_element_set_state(data.pipeline, GST_STATE_NULL);
    gst_object_unref(data.pipeline);
    g_main_loop_unref(data.loop);
    return 0;
}