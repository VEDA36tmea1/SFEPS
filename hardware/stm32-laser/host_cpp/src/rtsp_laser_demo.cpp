#include "vision_detector.h"
#include "target_provider.h"
#include "ibvs_controller.h"

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

// ─── LUT 그리드 설정 ───────────────────────────────────────────────────────
static constexpr int   LUT_GRID_ROWS = 11;
static constexpr int   LUT_GRID_COLS = 19;
static constexpr const char* LUT_ACK_FIFO = "/tmp/lut_ack";

static constexpr int   BOX_HALF     = 25;   // 타겟 박스 반지름(px)
static constexpr int   SEND_EVERY_N = 3;    // 파이프 전송 주기(프레임)

static bool g_lut_mode  = true;
static bool g_auto_lut  = false;  // --lut-auto

// ══════════════════════════════════════════════════════════════════════════
// AutoLutCalibrator  –  ACK 올 때까지 현재 그리드 유지, ACK 오면 다음 셀
//                       LUT 데이터 저장은 라즈베리(STM32) 쪽에서 수행
// ══════════════════════════════════════════════════════════════════════════
struct AutoLutCalibrator
{
    int rows, cols;
    int idx{0};
    bool done{false};
    int fifo_fd{-1};

    void init()
    {
        ::mkfifo(LUT_ACK_FIFO, 0666);
        fifo_fd = ::open(LUT_ACK_FIFO, O_RDONLY | O_NONBLOCK);
        if (fifo_fd < 0)
            std::cerr << "[AutoLUT] FIFO 열기 실패: "
                      << std::strerror(errno) << "\n";

        std::cerr << "[AutoLUT] 시작: " << rows * cols << " 포인트 순회\n"
                  << "[AutoLUT] ACK 대기 중 (STM32에서 SAVED/ACK 전송 시 다음 셀)\n";
    }

    int gr() const { return idx / cols; }
    int gc() const { return idx % cols; }
    int total() const { return rows * cols; }

    TargetROI currentTarget(int W, int H) const
    {
        int cx = static_cast<int>((gc() + 0.5f) * W / cols);
        int cy = static_cast<int>((gr() + 0.5f) * H / rows);
        TargetROI roi;
        roi.rect  = cv::Rect(cx - BOX_HALF, cy - BOX_HALF,
                              BOX_HALF * 2, BOX_HALF * 2);
        roi.valid = true;
        return roi;
    }

    // FIFO 에서 ACK/SAVED 수신 → true
    bool checkAck()
    {
        if (fifo_fd < 0) return false;
        char buf[256];
        int n = static_cast<int>(::read(fifo_fd, buf, sizeof(buf) - 1));
        if (n > 0)
        {
            buf[n] = '\0';
            return std::strstr(buf, "ACK")   != nullptr ||
                   std::strstr(buf, "SAVED") != nullptr;
        }
        return false;
    }

    // ACK 왔으면 다음 셀로 이동. 이동했으면 true 리턴 → 즉시 전송 트리거
    bool poll()
    {
        if (done) return false;
        if (!checkAck()) return false;

        std::cerr << "[AutoLUT] ACK ← GR=" << gr() << " GC=" << gc()
                  << "  (" << (idx + 1) << "/" << total() << ")\n";
        ++idx;
        if (idx >= total()) done = true;
        return true;
    }

    ~AutoLutCalibrator()
    {
        if (fifo_fd >= 0) ::close(fifo_fd);
    }
};

// ── 진행 오버레이 ──────────────────────────────────────────────────────────
static void drawLutOverlay(cv::Mat& frame, const AutoLutCalibrator& lut)
{
    int total = lut.total();
    int done  = lut.idx;
    double pct = total > 0 ? done * 100.0 / total : 0.0;

    cv::Mat bg = frame(cv::Rect(0, 0, 340, 36));
    bg *= 0.4;

    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "LUT [%d/%d] %.0f%%  GR=%d GC=%d  (ACK 대기)",
                  done, total, pct, lut.gr(), lut.gc());
    cv::putText(frame, buf, cv::Point(6, 24),
                cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(0, 255, 255), 1);
}

// ══════════════════════════════════════════════════════════════════════════
// main
// ══════════════════════════════════════════════════════════════════════════
int main(int argc, char** argv)
{
    std::string uri = "rtsp://admin:CCgbdCCgbd@192.168.0.84/profile2/media.smp";

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if      (arg == "--nolut")    g_lut_mode = false;
        else if (arg == "--lut-auto") g_auto_lut = true;
        else                          uri = arg;
    }

    std::cerr << "[rtsp_laser_demo] open: " << uri << "\n";
    std::cerr << "[rtsp_laser_demo] LUT=" << (g_lut_mode ? "ON" : "OFF")
              << "  자동순회=" << (g_auto_lut ? "ON" : "OFF") << "\n";

    cv::VideoCapture cap(uri);
    if (!cap.isOpened())
    {
        std::cerr << "[rtsp_laser_demo] RTSP 열기 실패\n";
        return 1;
    }

    VisionDetector  detector;
    IbvsController  controller(800, 2200, 1500, 0.02, -0.02);

    cv::namedWindow("rtsp_laser_demo");

    std::unique_ptr<ITargetProvider> targetProvider =
        std::make_unique<InteractiveBoxTargetProvider>(10, 10);
    if (auto* wa = dynamic_cast<IWindowAttachable*>(targetProvider.get()))
        wa->attachToWindow("rtsp_laser_demo");

    AutoLutCalibrator lut;
    lut.rows = LUT_GRID_ROWS;
    lut.cols = LUT_GRID_COLS;
    if (g_auto_lut)
        lut.init();

    cv::Mat     frame;
    int         frame_id = 0;
    cv::Point2f prev_laser(-1.f, -1.f);
    auto        last_time = std::chrono::steady_clock::now();

    if (!g_auto_lut)
        std::cerr << "[rtsp_laser_demo] 빈 곳 드래그: 박스 생성, 박스 안 드래그: 박스 이동\n";

    while (true)
    {
        if (!cap.read(frame) || frame.empty())
        {
            std::cerr << "[rtsp_laser_demo] empty frame\n";
            break;
        }

        auto now_tp = std::chrono::steady_clock::now();
        double dt   = std::chrono::duration_cast<std::chrono::duration<double>>(
                          now_tp - last_time).count();
        last_time   = now_tp;
        ++frame_id;

        int W = frame.cols;
        int H = frame.rows;

        // ── 1. Auto LUT: ACK 폴링 → 다음 셀 이동 ────────────────────
        bool lut_just_advanced = false;
        if (g_auto_lut)
            lut_just_advanced = lut.poll();

        // ── 2. 타겟 ROI ──────────────────────────────────────────────
        TargetROI targetROI;
        if (g_auto_lut)
        {
            if (lut.done)
            {
                std::cerr << "[AutoLUT] 전체 순회 완료!\n";
                break;
            }
            targetROI = lut.currentTarget(W, H);
        }
        else
        {
            targetROI = targetProvider->getTarget(frame);
        }

        if (targetROI.valid)
        {
            cv::rectangle(frame, targetROI.rect, cv::Scalar(0, 255, 0), 2);
            cv::circle(frame, targetROI.center(), 3, cv::Scalar(0, 255, 0), -1);
        }

        // ── 3. LUT 그리드 오버레이 ──────────────────────────────────
        if (g_lut_mode)
        {
            for (int c = 1; c < LUT_GRID_COLS; ++c)
            {
                int x = c * W / LUT_GRID_COLS;
                cv::line(frame, cv::Point(x, 0), cv::Point(x, H),
                         cv::Scalar(60, 60, 60), 1);
            }
            for (int r = 1; r < LUT_GRID_ROWS; ++r)
            {
                int y = r * H / LUT_GRID_ROWS;
                cv::line(frame, cv::Point(0, y), cv::Point(W, y),
                         cv::Scalar(60, 60, 60), 1);
            }

            if (g_auto_lut)
            {
                // 완료 셀 (dim 초록)
                for (int i = 0; i < lut.idx; ++i)
                {
                    int r = i / LUT_GRID_COLS, c = i % LUT_GRID_COLS;
                    int x0 = c * W / LUT_GRID_COLS, x1 = (c+1) * W / LUT_GRID_COLS;
                    int y0 = r * H / LUT_GRID_ROWS, y1 = (r+1) * H / LUT_GRID_ROWS;
                    cv::rectangle(frame, cv::Rect(x0, y0, x1-x0, y1-y0),
                                  cv::Scalar(0, 80, 0), -1);
                }
                // 현재 셀 (노랑)
                {
                    int r = lut.gr(), c = lut.gc();
                    int x0 = c * W / LUT_GRID_COLS, x1 = (c+1) * W / LUT_GRID_COLS;
                    int y0 = r * H / LUT_GRID_ROWS, y1 = (r+1) * H / LUT_GRID_ROWS;
                    cv::rectangle(frame, cv::Rect(x0, y0, x1-x0, y1-y0),
                                  cv::Scalar(0, 255, 255), 2);
                }
            }
        }

        // ── 4. 레이저 검출 ──────────────────────────────────────────
        DetectionResult laser = detector.detectLaser(frame);
        if (laser.found)
        {
            float dx = laser.point.x - prev_laser.x;
            float dy = laser.point.y - prev_laser.y;
            if (prev_laser.x < 0.f || dx*dx + dy*dy > 16.f)
            {
                std::cerr << "[laser] frame " << frame_id
                          << " (" << laser.point.x << ", " << laser.point.y << ")\n";
                prev_laser = laser.point;
            }
            cv::circle(frame, laser.point, 5, cv::Scalar(0, 0, 255), -1);
        }

        // ── 5. 오차 계산 & 파이프 출력 ─────────────────────────────
        double target_u = targetROI.valid
            ? static_cast<double>(targetROI.center().x) : 0.0;
        double target_v = targetROI.valid
            ? static_cast<double>(targetROI.center().y) : 0.0;
        double e_u = 0.0, e_v = 0.0;
        if (targetROI.valid && laser.found)
        {
            e_u = target_u - static_cast<double>(laser.point.x);
            e_v = target_v - static_cast<double>(laser.point.y);
            IbvsOutput out = controller.update(e_u, e_v, dt);

            int grid_col = std::clamp(static_cast<int>(target_u * LUT_GRID_COLS / W),
                                      0, LUT_GRID_COLS - 1);
            int grid_row = std::clamp(static_cast<int>(target_v * LUT_GRID_ROWS / H),
                                      0, LUT_GRID_ROWS - 1);

            if (!g_auto_lut && g_lut_mode)
            {
                int x0 = grid_col * W / LUT_GRID_COLS;
                int x1 = (grid_col+1) * W / LUT_GRID_COLS;
                int y0 = grid_row * H / LUT_GRID_ROWS;
                int y1 = (grid_row+1) * H / LUT_GRID_ROWS;
                cv::rectangle(frame, cv::Rect(x0, y0, x1-x0, y1-y0),
                              cv::Scalar(0, 255, 255), 2);
            }

            std::cerr << "[ctrl] frame " << frame_id
                      << " target=(" << target_u << ", " << target_v << ")"
                      << " laser=(" << laser.point.x << ", " << laser.point.y << ")"
                      << " e=(" << e_u << ", " << e_v << ")"
                      << " pwm=(" << out.pan_us << ", " << out.tilt_us << ")\n";
        }

        // 전송: ACK 직후 다음 셀은 반드시 1회 전송, 그 외에는 (타겟+레이저 있을 때) 주기 전송
        int grid_col = std::clamp(static_cast<int>(target_u * LUT_GRID_COLS / W), 0, LUT_GRID_COLS - 1);
        int grid_row = std::clamp(static_cast<int>(target_v * LUT_GRID_ROWS / H), 0, LUT_GRID_ROWS - 1);
        bool should_send = lut_just_advanced
            || (targetROI.valid && laser.found && (frame_id % SEND_EVERY_N == 0));

        if (should_send && targetROI.valid)
        {
            std::cout << e_u      << " "
                      << e_v      << " "
                      << target_u << " "
                      << target_v << " "
                      << grid_row << " "
                      << grid_col << std::endl;
        }

        // ── 6. Auto LUT 오버레이 ───────────────────────────────────
        if (g_auto_lut && !lut.done)
            drawLutOverlay(frame, lut);

        cv::imshow("rtsp_laser_demo", frame);
        int key = cv::waitKey(1);
        if (key == 27 || key == 'q') break;
    }

    return 0;
}
