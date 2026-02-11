#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <iostream>
#include "log.h"
#include "auth.h"
#include <thread>
#include <arpa/inet.h>

// [수정 필요] 가져올 외부 카메라(CCTV)의 주소
#define EXTERNAL_RTSP_URL "rtsp://admin:CCgbdCCgbd@192.168.0.30/profile2/media.smp" 

// [설정] 서버 포트 및 경로 (rtsp://내IP:8554/live)
#define SERVER_PORT "8554"
#define MOUNT_POINT "/live"

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

// [추가] 로그인만 담당하는 전용 함수
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
    
    // 서버 시작 로그 기록
    myLogger.enqueue("SYSTEM", "SFEPS Relay Server Initializing...");
    std::cout << "DB Connected & Logger Initialized." << std::endl;

    std::thread auth_thread(run_login_auth);
    auth_thread.detach(); // 백그라운드에서 알아서 돌아가게 분리

    // ---------------------------------------------------------
    // 2. GStreamer RTSP 서버 설정
    // ---------------------------------------------------------
    GMainLoop *loop;
    GstRTSPServer *server;
    GstRTSPMountPoints *mounts;
    GstRTSPMediaFactory *factory;

    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);

    // 서버 생성
    server = gst_rtsp_server_new();
    g_object_set(server, "service", SERVER_PORT, NULL);

    // 마운트 포인트(주소) 관리자 가져오기
    mounts = gst_rtsp_server_get_mount_points(server);

    // 미디어 공장(Factory) 생성
    factory = gst_rtsp_media_factory_new();

    // ★ [핵심] 파이프라인 설정 (중계소 모드) ★
    // 외부 RTSP(rtspsrc) -> 포장 뜯기(depay) -> 정리(parse) -> 다시 포장(pay)
    // latency=0: 지연시간 최소화
   gst_rtsp_media_factory_set_launch(factory, 
        "( "
        "rtspsrc location=" EXTERNAL_RTSP_URL " protocols=tcp latency=500 ! "
        "rtph264depay ! "
        "h264parse ! "
        "rtph264pay name=pay0 pt=96 "
        ")");

    // 여러 명이 접속해도 공유하도록 설정
    gst_rtsp_media_factory_set_shared(factory, TRUE);

    // 주소 등록 (/live)
    gst_rtsp_mount_points_add_factory(mounts, MOUNT_POINT, factory);
    g_object_unref(mounts);

    // ---------------------------------------------------------
    // 3. 접속 감지 설정 (Signal 연결)
    // ---------------------------------------------------------
    ServerData data;
    data.logger = &myLogger;

    // 'client-connected' 신호가 오면 client_connected 함수 실행
    g_signal_connect(server, "client-connected", G_CALLBACK(client_connected), &data);

    // 서버 시작
    if (gst_rtsp_server_attach(server, NULL) == 0) {
        std::cerr << "[ERROR] Failed to attach server to port " << SERVER_PORT << std::endl;
        myLogger.enqueue("ERROR", "Failed to start RTSP Server");
        return -1;
    }

    std::cout << "------------------------------------------------" << std::endl;
    std::cout << " SFEPS Relay Server Running at rtsp://127.0.0.1:" << SERVER_PORT << MOUNT_POINT << std::endl;
    std::cout << " Monitoring External Cam: " << EXTERNAL_RTSP_URL << std::endl;
    std::cout << "------------------------------------------------" << std::endl;
    
    myLogger.enqueue("SYSTEM", "Server Running. Ready for clients.");

    // 무한 루프 (서버 가동)
    g_main_loop_run(loop);

    return 0;
}