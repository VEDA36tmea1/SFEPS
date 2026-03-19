#include <opencv2/opencv.hpp>
#include <iostream>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <csignal>
#include "../inc/img_processing.h"

// 0이면 로컬 이미지 테스트, 1이면 실제 카메라 가동
#define LIVE_CAMERA_MODE 1

std::atomic<bool> is_running{true};
std::mutex mtx_raw;
std::queue<cv::Mat> raw_queue;

std::condition_variable cv_rfid;
std::mutex mtx_rfid;
bool rfid_detected = false;

// RAW 모드 여부 (파이프라인 열기 성공 여부에 따라 런타임 결정)
bool g_raw_mode = false;

void signalHandler(int signum) {
    // write()는 async-signal-safe, cout은 아님 — 단순 출력만 사용
    const char msg[] = "\n[종료] 시스템을 종료합니다.\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);

    is_running = false;
    cv_rfid.notify_all();

    // cap.grab()은 GStreamer 내부에서 블로킹되어 release()로도 깨지 않음.
    // detach된 스레드가 자연 종료를 기다리지 않고 프로세스를 즉시 끝냄.
    // 파일/소켓 등 OS 자원은 커널이 자동 회수하므로 안전함.
    _exit(0);
}

#if LIVE_CAMERA_MODE

void captureThreadFunc(cv::VideoCapture& cap) {
    while (is_running) {
        cv::Mat tmp;
        // grab()은 내부적으로 appsink timeout을 타므로 블로킹이 짧게 끊깁니다.
        // is_running이 false가 되면 다음 grab() 반환 후 루프를 탈출합니다.
        if (!cap.grab()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        cap.retrieve(tmp);
        if (!tmp.empty()) {
            std::lock_guard<std::mutex> lock(mtx_raw);
            if (!raw_queue.empty()) raw_queue.pop();
            raw_queue.push(tmp);
        }
    }
}

void rfidThreadFunc() {
    int fd = open("/dev/rc522", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "❌ RFID 열기 실패 (sudo 권한 확인)" << std::endl;
        return;
    }
    uint8_t uid[4];
    while (is_running) {
        // select()로 최대 200ms 대기 → is_running 체크 주기
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv{0, 200000}; // 200ms
        int ret = select(fd + 1, &fds, nullptr, nullptr, &tv);
        if (ret <= 0) continue; // 타임아웃 or 에러 → 루프 재진입해서 is_running 확인

        if (read(fd, uid, sizeof(uid)) == 4) {
            std::cout << "💳 RFID 태그됨! UID: "
                      << std::hex << (int)uid[0] << ":" << (int)uid[1]
                      << std::dec << std::endl;
            {
                std::lock_guard<std::mutex> lock(mtx_rfid);
                rfid_detected = true;
            }
            cv_rfid.notify_one();
            // 중복 방지 대기도 is_running 체크하며 쪼개기
            for (int i = 0; i < 20 && is_running; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    close(fd);
}

// RAW 10-bit 파이프라인 (자체 ISP용)
// libcamera가 SRGGB10_CSI2P 패킹 포맷을 내보내므로 video/x-bayer로 수신
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

// -----------------------------------------------------------------
// 프레임을 받아 전체 ISP 파이프라인을 실행하고 결과를 저장
// raw_mode=true  → runPureISP (CV_16UC1) → processISPAndGetBest
// raw_mode=false → processISPAndGetBest only (CV_8UC3)
// -----------------------------------------------------------------
static void runFullPipeline(cv::Mat& frame, bool raw_mode) {
    cv::imwrite("1_raw_capture.jpg", frame);

    cv::Mat isp_out;
    if (raw_mode) {
        // [경로 A] 자체 ISP: BLC/AWB/AE → Demosaic → CCM → Gamma
        if (frame.type() != CV_16UC1) {
            std::cerr << "⚠️  RAW 모드인데 CV_16UC1이 아닙니다. BGR 경로로 폴백합니다." << std::endl;
            isp_out = frame;
        } else {
            std::cout << "🔧 자체 RAW ISP 실행 중..." << std::endl;
            isp_out = runPureISP(frame);
            cv::imwrite("2_pure_isp_out.jpg", isp_out);
        }
    } else {
        // [경로 B] libcamera ISP를 거친 BGR 이미지 그대로 사용
        isp_out = frame;
        std::cout << "ℹ️  BGR 모드: libcamera ISP 출력을 그대로 사용합니다." << std::endl;
    }

    // [공통] ShadowBoost + CLAHE → Entropy 기반 Best-shot 선택
    std::cout << "📐 ShadowBoost/CLAHE 후처리 실행 중..." << std::endl;
    cv::Mat tuning_view;
    cv::Mat best_frame = processISPAndGetBest(isp_out, tuning_view);

    cv::imwrite("3_tuning_viewer.jpg", tuning_view);
    cv::imwrite("4_best_shot.jpg", best_frame);
    std::cout << "✅ 저장 완료! (1_raw / 2_isp / 3_tuning / 4_best)" << std::endl;
}

int main() {
    signal(SIGINT, signalHandler);

#if LIVE_CAMERA_MODE

    // --- RAW 파이프라인 우선 시도 ---
    cv::VideoCapture cap(PIPE_RAW, cv::CAP_GSTREAMER);
    if (cap.isOpened()) {
        g_raw_mode = true;
        std::cout << "✅ RAW 파이프라인 열기 성공 → 자체 ISP 사용" << std::endl;
    } else {
        std::cout << "⚠️  RAW 파이프라인 실패 → BGR 폴백 시도..." << std::endl;
        cap.open(PIPE_BGR, cv::CAP_GSTREAMER);
        if (!cap.isOpened()) {
            std::cerr << "❌ BGR 파이프라인도 실패. 카메라를 확인하세요." << std::endl;
            return -1;
        }
        g_raw_mode = false;
        std::cout << "✅ BGR 파이프라인 열기 성공 → libcamera ISP 사용" << std::endl;
    }

    std::thread cap_thread(captureThreadFunc, std::ref(cap));
    std::thread rfid_thread(rfidThreadFunc);

    while (is_running) {
        std::cout << "⌛ 태그 대기 중... (모드: "
                  << (g_raw_mode ? "RAW 자체ISP" : "BGR libcamera") << ")" << std::endl;

        std::unique_lock<std::mutex> lock(mtx_rfid);
        cv_rfid.wait(lock, [] { return rfid_detected || !is_running; });
        if (!is_running) break;
        rfid_detected = false;

        cv::Mat target_frame;
        {
            std::lock_guard<std::mutex> lock_raw(mtx_raw);
            if (!raw_queue.empty()) {
                target_frame = raw_queue.front().clone();
                raw_queue.pop(); // 처리 후 명시적으로 제거
            }
        }
        if (target_frame.empty()) {
            std::cerr << "⚠️  프레임이 아직 준비되지 않았습니다." << std::endl;
            continue;
        }

        std::cout << "📸 찰칵! ISP 파이프라인 시작..." << std::endl;
        runFullPipeline(target_frame, g_raw_mode);
    }

    // _exit()가 호출되므로 join 대신 detach — 블로킹 없이 즉시 반환
    cap_thread.detach();
    rfid_thread.detach();
    cap.release();

#else
    // --- 로컬 테스트 모드 ---
    // 16-bit RAW 파일이면 자체 ISP, 8-bit 이미지면 BGR 경로
    cv::Mat target_frame = cv::imread("img/test_image4.jpg", cv::IMREAD_UNCHANGED);
    if (target_frame.empty()) {
        std::cerr << "❌ 이미지 로드 실패" << std::endl;
        return -1;
    }

    bool local_raw_mode = (target_frame.type() == CV_16UC1);
    std::cout << "🖼️  로컬 테스트 모드 (타입: "
              << (local_raw_mode ? "CV_16UC1 RAW" : "CV_8UC3 BGR") << ")" << std::endl;

    runFullPipeline(target_frame, local_raw_mode);
#endif

    std::cout << "\n✅ 시스템이 안전하게 종료되었습니다." << std::endl;
    return 0;
}   