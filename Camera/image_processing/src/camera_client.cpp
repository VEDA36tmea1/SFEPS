// camera_client.cpp
// ─────────────────────────────────────────────────────────────────────
// 단일 프로세스, 스레드 3개
//   [1] captureThread  : 카메라 프레임 캡처 → raw_queue (최신 1장 유지)
//   [2] rfidThread     : /tmp/rc522_events.sock 수신 → pipeline_queue push
//   [3] pipelineThread : pipeline_queue에서 꺼내 ISP 처리
//
// rc522 데몬이 /dev/rc522 를 읽고 소켓으로 JSON을 내보냄.
// 이 프로세스는 소켓 클라이언트로만 동작 — 하드웨어 직접 접근 없음.
// ─────────────────────────────────────────────────────────────────────

#include <opencv2/opencv.hpp>
#include <iostream>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <string>

#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "../inc/img_processing.h"

// 0이면 로컬 이미지 테스트, 1이면 실제 카메라 가동
#define LIVE_CAMERA_MODE 1

static constexpr const char* RC522_SOCK_PATH   = "/tmp/rc522_events.sock";
static constexpr int         PIPELINE_QUEUE_MAX = 5; // 태그 이벤트 최대 누적 수

std::atomic<bool> g_running{true};
bool              g_raw_mode = false;

// ── 카메라 프레임 큐 (captureThread → rfidThread) ─────────────────────
std::mutex          mtx_raw;
std::queue<cv::Mat> raw_queue; // 최신 1장만 유지

// ── 파이프라인 큐 (rfidThread → pipelineThread) ───────────────────────
std::mutex              mtx_pipeline;
std::condition_variable cv_pipeline;
std::queue<cv::Mat>     pipeline_queue;

// ──────────────────────────────────────────────────────────────────────
// 시그널 핸들러 (Ctrl+C)
// ──────────────────────────────────────────────────────────────────────
void signalHandler(int /*signum*/) {
    const char msg[] = "\n[종료] 시스템을 종료합니다.\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    _exit(0);
}

#if LIVE_CAMERA_MODE

// ──────────────────────────────────────────────────────────────────────
// [스레드 1] 카메라 캡처
// ──────────────────────────────────────────────────────────────────────
void captureThreadFunc(cv::VideoCapture& cap) {
    while (g_running) {
        if (!cap.grab()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        cv::Mat tmp;
        cap.retrieve(tmp);
        if (!tmp.empty()) {
            std::lock_guard<std::mutex> lock(mtx_raw);
            while (!raw_queue.empty()) raw_queue.pop();
            raw_queue.push(std::move(tmp));
        }
    }
}

// ──────────────────────────────────────────────────────────────────────
// 태그 이벤트 발생 시 호출 — 프레임 스냅샷 후 pipeline_queue push
// ──────────────────────────────────────────────────────────────────────
static void onTagDetected(const std::string& uid) {
    cv::Mat snapshot;
    {
        std::lock_guard<std::mutex> lock(mtx_raw);
        if (!raw_queue.empty())
            snapshot = raw_queue.front().clone();
    }
    if (snapshot.empty()) {
        std::cerr << "[rfid] ⚠️  프레임 미준비 — 이벤트 버림 (uid=" << uid << ")" << std::endl;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mtx_pipeline);
        if ((int)pipeline_queue.size() >= PIPELINE_QUEUE_MAX) {
            std::cerr << "[rfid] ⚠️  큐 가득 참 — 이벤트 버림 (uid=" << uid << ")" << std::endl;
            return;
        }
        pipeline_queue.push(std::move(snapshot));
        std::cout << "[rfid] 📥 큐 적재: "
                  << pipeline_queue.size() << "/" << PIPELINE_QUEUE_MAX
                  << "  uid=" << uid << std::endl;
    }
    cv_pipeline.notify_one();
}

// ──────────────────────────────────────────────────────────────────────
// 간단한 JSON 파서 — "key": "value" 추출
// rfid_monitor.cpp 와 동일한 방식
// ──────────────────────────────────────────────────────────────────────
static std::string extractJsonValue(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    size_t kpos = json.find(token);
    if (kpos == std::string::npos) return "";
    size_t cursor = json.find(':', kpos + token.size());
    if (cursor == std::string::npos) return "";
    ++cursor;
    while (cursor < json.size() && std::isspace((unsigned char)json[cursor])) ++cursor;
    if (cursor >= json.size()) return "";
    if (json[cursor] == '"') {
        ++cursor;
        std::string val;
        for (; cursor < json.size(); ++cursor) {
            if (json[cursor] == '\\') { ++cursor; val.push_back(json[cursor]); continue; }
            if (json[cursor] == '"') break;
            val.push_back(json[cursor]);
        }
        return val;
    }
    size_t end = json.find_first_of(",}", cursor);
    std::string val = (end == std::string::npos) ? json.substr(cursor)
                                                  : json.substr(cursor, end - cursor);
    // trim
    size_t s = val.find_first_not_of(" \t\r\n");
    size_t e = val.find_last_not_of(" \t\r\n");
    return (s == std::string::npos) ? "" : val.substr(s, e - s + 1);
}

// ──────────────────────────────────────────────────────────────────────
// [스레드 2] RFID 이벤트 수신
// /tmp/rc522_events.sock 에 클라이언트로 접속 (rfid_monitor.cpp 와 동일 구조)
// 연결 끊기면 1초 후 자동 재접속
// ──────────────────────────────────────────────────────────────────────
void rfidThreadFunc() {
    while (g_running) {
        // ── 소켓 연결 ─────────────────────────────────────────────────
        int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock_fd < 0) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, RC522_SOCK_PATH, sizeof(addr.sun_path) - 1);

        if (connect(sock_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
            // rc522 데몬 미실행 시 조용히 재시도
            close(sock_fd);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        std::cout << "[rfid] ✅ rc522 데몬 연결 성공" << std::endl;

        // ── 수신 루프 ─────────────────────────────────────────────────
        char buf[4096];
        std::string line_buf;

        while (g_running) {
            struct pollfd pfd{sock_fd, POLLIN, 0};
            int ret = poll(&pfd, 1, 1000); // 1초 타임아웃 → g_running 체크

            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (ret == 0) continue; // 타임아웃

            if (pfd.revents & (POLLERR | POLLHUP)) {
                std::cerr << "[rfid] 소켓 끊김. 재접속 시도..." << std::endl;
                break;
            }
            if (!(pfd.revents & POLLIN)) continue;

            ssize_t n = read(sock_fd, buf, sizeof(buf) - 1);
            if (n == 0) {
                std::cerr << "[rfid] 데몬 연결 종료. 재접속 시도..." << std::endl;
                break;
            }
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }

            buf[n] = '\0';
            line_buf += buf;

            // NDJSON: '\n' 단위로 파싱
            size_t pos;
            while ((pos = line_buf.find('\n')) != std::string::npos) {
                std::string line = line_buf.substr(0, pos);
                line_buf.erase(0, pos + 1);
                if (line.empty()) continue;

                std::string uid = extractJsonValue(line, "id");
                if (uid.empty()) continue; // 필수 필드 없으면 무시

                std::cout << "[rfid] 💳 태그: uid=" << uid << std::endl;
                onTagDetected(uid);
            }
        }

        close(sock_fd);
        if (g_running)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::cout << "[rfid] 스레드 종료" << std::endl;
}

// ──────────────────────────────────────────────────────────────────────
// ISP 파이프라인
// ──────────────────────────────────────────────────────────────────────
static void runFullPipeline(cv::Mat& frame, bool raw_mode) {
    cv::imwrite("1_raw_capture.jpg", frame);

    cv::Mat isp_out;
    if (raw_mode) {
        if (frame.type() != CV_16UC1) {
            std::cerr << "⚠️  CV_16UC1 아님 — BGR 폴백" << std::endl;
            isp_out = frame;
        } else {
            std::cout << "🔧 자체 RAW ISP 실행 중..." << std::endl;
            isp_out = runPureISP(frame);
            cv::imwrite("2_pure_isp_out.jpg", isp_out);
        }
    } else {
        isp_out = frame;
        std::cout << "ℹ️  BGR 모드: libcamera ISP 출력 사용" << std::endl;
    }

    std::cout << "📐 ShadowBoost/CLAHE 후처리 실행 중..." << std::endl;
    cv::Mat tuning_view;
    cv::Mat best_frame = processISPAndGetBest(isp_out, tuning_view);

    cv::imwrite("3_tuning_viewer.jpg", tuning_view);
    cv::imwrite("4_best_shot.jpg", best_frame);
    std::cout << "✅ 저장 완료 (1_raw / 2_isp / 3_tuning / 4_best)" << std::endl;
}

// ──────────────────────────────────────────────────────────────────────
// [스레드 3] ISP 파이프라인 처리
// ──────────────────────────────────────────────────────────────────────
void pipelineThreadFunc() {
    while (g_running) {
        cv::Mat frame;
        {
            std::unique_lock<std::mutex> lock(mtx_pipeline);
            cv_pipeline.wait(lock, [] {
                return !pipeline_queue.empty() || !g_running;
            });
            if (!g_running && pipeline_queue.empty()) break;

            frame = std::move(pipeline_queue.front());
            pipeline_queue.pop();
            std::cout << "[pipeline] 🔄 큐 잔여: "
                      << pipeline_queue.size() << "/" << PIPELINE_QUEUE_MAX << std::endl;
        }
        std::cout << "📸 ISP 파이프라인 시작..." << std::endl;
        runFullPipeline(frame, g_raw_mode);
    }
    std::cout << "[pipeline] 스레드 종료" << std::endl;
}

static const std::string PIPE_RAW =
    "libcamerasrc ! "
    "video/x-raw,format=SRGGB10,width=1920,height=1080,framerate=30/1 ! "
    "appsink drop=true max-buffers=1 emit-signals=false wait-on-eos=false";

static const std::string PIPE_BGR =
    "libcamerasrc ! "
    "video/x-raw,width=1920,height=1080,framerate=30/1 ! "
    "videoconvert ! video/x-raw,format=BGR ! "
    "appsink drop=true max-buffers=1 emit-signals=false wait-on-eos=false";

#endif // LIVE_CAMERA_MODE

int main() {
    signal(SIGINT,  signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGPIPE, SIG_IGN);

#if LIVE_CAMERA_MODE

    cv::VideoCapture cap(PIPE_RAW, cv::CAP_GSTREAMER);
    if (cap.isOpened()) {
        g_raw_mode = true;
        std::cout << "✅ RAW 파이프라인 성공 → 자체 ISP 사용" << std::endl;
    } else {
        std::cout << "⚠️  RAW 실패 → BGR 폴백..." << std::endl;
        cap.open(PIPE_BGR, cv::CAP_GSTREAMER);
        if (!cap.isOpened()) {
            std::cerr << "❌ BGR 파이프라인도 실패. 카메라 확인 필요." << std::endl;
            return -1;
        }
        g_raw_mode = false;
        std::cout << "✅ BGR 파이프라인 성공 → libcamera ISP 사용" << std::endl;
    }

    std::thread cap_thread(captureThreadFunc, std::ref(cap));
    std::thread rfid_thread(rfidThreadFunc);
    std::thread pipeline_thread(pipelineThreadFunc);

    while (g_running)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    cap_thread.detach();
    rfid_thread.detach();
    pipeline_thread.detach();
    cap.release();

#else
    cv::Mat target_frame = cv::imread("img/test_image4.jpg", cv::IMREAD_UNCHANGED);
    if (target_frame.empty()) {
        std::cerr << "❌ 이미지 로드 실패" << std::endl;
        return -1;
    }
    bool local_raw_mode = (target_frame.type() == CV_16UC1);
    std::cout << "🖼️  로컬 테스트 (타입: "
              << (local_raw_mode ? "CV_16UC1 RAW" : "CV_8UC3 BGR") << ")" << std::endl;
    runFullPipeline(target_frame, local_raw_mode);
#endif

    return 0;
}