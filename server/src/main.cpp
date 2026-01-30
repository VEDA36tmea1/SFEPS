#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <iostream>
#include "db_handler.h" // 우리가 만든 헤더 파일 포함

int main(int argc, char *argv[]) {
    // 1. DB 연결 및 로그 기록
    DBHandler db;
    if (db.connect()) {
        db.writeLog("Server Started (Refactored Structure)");
    }

    // 2. GStreamer RTSP 서버 설정
    gst_init(&argc, &argv);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    GstRTSPServer *server = gst_rtsp_server_new();
    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server);
    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();

    // libcamerasrc 파이프라인
    gst_rtsp_media_factory_set_launch(factory, 
        "( "
        "libcamerasrc ! "
        "video/x-raw,width=1280,height=720,framerate=30/1 ! "
        "videoconvert ! "
        "x264enc tune=zerolatency speed-preset=ultrafast bitrate=2500 key-int-max=30 sliced-threads=true ! "
        "h264parse config-interval=-1 ! "
        "rtph264pay name=pay0 pt=96 "
        ")");

    gst_rtsp_media_factory_set_latency(factory, 0);

    gst_rtsp_media_factory_set_shared(factory, TRUE);
    gst_rtsp_mount_points_add_factory(mounts, "/live", factory);
    
    g_object_unref(mounts);
    gst_rtsp_server_attach(server, NULL);

    std::cout << "========================================" << std::endl;
    std::cout << "   RTSP Smart Server (구조개선판) 시작   " << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "VLC 주소: rtsp://<IP>:8554/live" << std::endl;

    g_main_loop_run(loop);

    return 0;
}
