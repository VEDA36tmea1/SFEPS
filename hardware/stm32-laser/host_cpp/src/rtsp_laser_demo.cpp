#include "vision_detector.h"
#include "target_provider.h"
#include "ibvs_controller.h"

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <map>

// ─── LUT 그리드 설정 ───────────────────────────────────────────────────────
static constexpr int   LUT_GRID_ROWS = 11;
static constexpr int   LUT_GRID_COLS = 19;
static constexpr const char* LUT_CLIENT_CONNECTED_FIFO = "/tmp/lut_client_connected";
static constexpr const char* LUT_PWM_RESPONSE_FIFO     = "/tmp/lut_pwm_response";

static constexpr int   BOX_HALF     = 25;   // 타겟 박스 반지름(px)
static constexpr int   SEND_EVERY_N = 3;    // 파이프 전송 주기(프레임)
static constexpr float SETTLE_THRESH_PX = 6.0f; // 수렴 판정 픽셀 오차
static constexpr float SETTLE_TIME_SEC  = 3.0f; // 수렴 유지 시간(초)

static bool g_lut_mode  = true;
static bool g_auto_lut  = false;  // --lut-auto

// AutoLUT 중 레이저 미검출/오검출 시 수동으로 레이저 위치를 찍어서 저장하기 위한 클릭 포인트
static bool g_manual_laser_pending = false;
static cv::Point g_manual_laser_pt(0, 0);

static void onMouseManualLaser(int event, int x, int y, int /*flags*/, void* /*userdata*/)
{
    if (!g_auto_lut)
        return;
    if (event == cv::EVENT_LBUTTONDOWN)
    {
        g_manual_laser_pt = cv::Point(x, y);
        g_manual_laser_pending = true;
    }
}

// ══════════════════════════════════════════════════════════════════════════
// AutoLutCalibrator  –  서버(host)에서 수렴 판정(±6px, 3초) 후
//                       라즈베리에 PWM 요청 → 응답 수신 → JSON 저장 → 다음 셀
// ══════════════════════════════════════════════════════════════════════════
struct AutoLutCalibrator
{
    int rows, cols;
    int idx{0};
    bool done{false};
    int pwm_fifo_fd{-1};
    std::string json_path{"lut_data.json"};
    std::vector<std::string> points_json;
    // 각 그리드 셀 → points_json 내 인덱스 (덮어쓰기용)
    std::map<std::pair<int,int>, size_t> cell_index;

    double settle_accum_s{0.0}; // 연속 수렴 시간 누적

    void init(const std::string& path = "")
    {
        if (!path.empty())
            json_path = path;

        ::mkfifo(LUT_PWM_RESPONSE_FIFO, 0666);
        pwm_fifo_fd = ::open(LUT_PWM_RESPONSE_FIFO, O_RDONLY | O_NONBLOCK);
        if (pwm_fifo_fd < 0)
            std::cerr << "[AutoLUT] PWM FIFO 열기 실패: " << std::strerror(errno) << "\n";

        // 기존 LUT 파일이 있으면 로드 → 저장된 셀 이후부터 이어서 진행
        std::ifstream ifs(json_path);
        if (ifs)
        {
            std::string line;
            while (std::getline(ifs, line))
            {
                if (line.find("\"grid_r\"") != std::string::npos &&
                    line.find("\"pan_us\"") != std::string::npos)
                {
                    while (!line.empty() && (line.back() == ',' || line.back() == '\r' || line.back() == ' '))
                        line.pop_back();
                    size_t start = line.find_first_not_of(" \t\r\n");
                    if (start != std::string::npos)
                    {
                        std::string t = line.substr(start);
                        if (!t.empty() && t.front() == '{')
                            t = "  " + t;
                        points_json.push_back(t);

                        // grid_r,grid_c 파싱해서 cell_index 맵도 복원
                        int gr = -1, gc = -1;
                        if (std::sscanf(t.c_str(), "  {\"grid_r\":%d,\"grid_c\":%d", &gr, &gc) == 2)
                        {
                            std::pair<int,int> key{gr, gc};
                            cell_index[key] = points_json.size() - 1;
                        }
                    }
                }
            }
            idx = static_cast<int>(points_json.size());
            if (idx > 0)
                std::cerr << "[AutoLUT] 기존 " << json_path << " 로드: " << idx << "개 셀 → 다음 셀부터 진행\n";
            if (idx >= total())
                done = true;
        }

        std::cerr << "[AutoLUT] 시작: " << rows * cols << " 포인트 순회\n"
                  << "[AutoLUT] 수렴: |e|≤" << SETTLE_THRESH_PX << "px × " << SETTLE_TIME_SEC << "초\n"
                  << "[AutoLUT] 저장: " << json_path << "\n";
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

    // 수렴 판정: |e_u|,|e_v|≤6px 연속 유지 시간 누적
    bool updateConvergence(double e_u, double e_v, double dt_s)
    {
        if (std::fabs(e_u) <= SETTLE_THRESH_PX && std::fabs(e_v) <= SETTLE_THRESH_PX)
            settle_accum_s += dt_s;
        else
            settle_accum_s = 0.0;
        return settle_accum_s >= SETTLE_TIME_SEC;
    }

    // REQUEST_PWM 전송 → FIFO에서 PWM 수신 → JSON 저장 → 다음 셀. 성공 시 true
    bool requestPwmAndSave(double target_u, double target_v, std::ostream& pipe_out)
    {
        if (done) return false;
        int gr_val = gr(), gc_val = gc();

        pipe_out << "REQUEST_PWM,GR=" << gr_val << ",GC=" << gc_val << std::endl;
        pipe_out.flush();

        // FIFO에서 PAN=...,TILT=... 수신 대기 (최대 ~2초 폴링)
        float pan_us = 0.f, tilt_us = 0.f;
        int poll_count = 0;
        while (poll_count < 200 && pwm_fifo_fd >= 0)
        {
            char buf[256];
            int n = static_cast<int>(::read(pwm_fifo_fd, buf, sizeof(buf) - 1));
            if (n > 0)
            {
                buf[n] = '\0';
                if (std::sscanf(buf, "PAN=%f,TILT=%f", &pan_us, &tilt_us) >= 2 ||
                    std::sscanf(buf, "GR=%*d,GC=%*d,PAN=%f,TILT=%f", &pan_us, &tilt_us) >= 2)
                {
                    break;
                }
            }
            usleep(10000);  // 10ms
            ++poll_count;
        }

        if (pan_us == 0.f && tilt_us == 0.f)
        {
            std::cerr << "[AutoLUT] PWM 수신 타임아웃 GR=" << gr_val << " GC=" << gc_val << "\n";
            settle_accum_s = 0.0;
            return false;
        }

        // JSON 포인트 추가
        std::ostringstream oss;
        oss << "  {\"grid_r\":" << gr_val << ",\"grid_c\":" << gc_val
            << ",\"target_u\":" << std::fixed << std::setprecision(1) << target_u
            << ",\"target_v\":" << target_v
            << ",\"pan_us\":" << std::setprecision(0) << pan_us
            << ",\"tilt_us\":" << tilt_us << "}";
        std::string point_str = oss.str();

        // 같은 셀(grid_r,grid_c)이 이미 있으면 덮어쓰기, 아니면 새로 추가
        std::pair<int,int> key{gr_val, gc_val};
        auto it = cell_index.find(key);
        if (it != cell_index.end())
        {
            points_json[it->second] = point_str;
            std::cerr << "[AutoLUT] UPDATED GR=" << gr_val << " GC=" << gc_val
                      << " pan=" << pan_us << " tilt=" << tilt_us << "\n";
        }
        else
        {
            cell_index[key] = points_json.size();
            points_json.push_back(point_str);
        }

        // 파일 저장 (그리드 1개 저장 시마다 즉시 기록)
        std::ofstream ofs(json_path);
        if (ofs)
        {
            ofs << "{\n  \"rows\":" << rows << ",\n  \"cols\":" << cols << ",\n  \"points\":[\n";
            for (size_t i = 0; i < points_json.size(); ++i)
            {
                ofs << points_json[i];
                if (i + 1 < points_json.size()) ofs << ",";
                ofs << "\n";
            }
            ofs << "  ],\n  \"count\":" << points_json.size()
                << ",\n  \"total\":" << total() << "\n}\n";
            ofs.flush();
        }

        std::cerr << "[AutoLUT] SAVED GR=" << gr_val << " GC=" << gc_val
                  << " pan=" << pan_us << " tilt=" << tilt_us
                  << "  (" << (idx + 1) << "/" << total() << ")\n";

        settle_accum_s = 0.0;
        ++idx;
        if (idx >= total()) done = true;
        return true;
    }

    ~AutoLutCalibrator()
    {
        if (pwm_fifo_fd >= 0) ::close(pwm_fifo_fd);
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
                  "LUT [%d/%d] %.0f%%  GR=%d GC=%d  settle %.1f/%.1fs",
                  done, total, pct, lut.gr(), lut.gc(),
                  lut.settle_accum_s, (double)SETTLE_TIME_SEC);
    cv::putText(frame, buf, cv::Point(6, 24),
                cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(0, 255, 255), 1);
}

// ══════════════════════════════════════════════════════════════════════════
// main
// ══════════════════════════════════════════════════════════════════════════
int main(int argc, char** argv)
{
    std::string uri = "rtsp://admin:CCgbdCCgbd@192.168.0.22/profile2/media.smp";

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
    // AutoLUT에서는 ROI가 자동이므로, 레이저 수동 클릭을 위해 마우스 콜백을 덮어쓴다.
    if (g_auto_lut)
        cv::setMouseCallback("rtsp_laser_demo", onMouseManualLaser, nullptr);

    AutoLutCalibrator lut;
    lut.rows = LUT_GRID_ROWS;
    lut.cols = LUT_GRID_COLS;
    if (g_auto_lut)
        lut.init("");

    cv::Mat     frame;
    int         frame_id = 0;
    cv::Point2f prev_laser(-1.f, -1.f);
    auto        last_time = std::chrono::steady_clock::now();

    if (!g_auto_lut)
        std::cerr << "[rtsp_laser_demo] 빈 곳 드래그: 박스 생성, 박스 안 드래그: 박스 이동\n";
    if (g_auto_lut)
        std::cerr << "[rtsp_laser_demo] AutoLUT: 레이저 미검출 시 좌클릭으로 레이저 위치 지정 → PWM 요청/저장/다음 셀\n";

    // 라즈베리(클라이언트) 연결 시에만 LUT 요청/전송 시작 (FIFO로 신호 수신)
    bool client_connected = false;
    int  client_conn_fifo_fd = -1;
    ::mkfifo(LUT_CLIENT_CONNECTED_FIFO, 0666);
    client_conn_fifo_fd = ::open(LUT_CLIENT_CONNECTED_FIFO, O_RDONLY | O_NONBLOCK);
    if (client_conn_fifo_fd < 0)
        std::cerr << "[rtsp_laser_demo] 클라이언트 연결 FIFO 열기 실패: "
                  << std::strerror(errno) << "\n";
    else
        std::cerr << "[rtsp_laser_demo] 라즈베리 파이 연결 대기 중... (연결되면 LUT 동작 시작)\n";

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

        // ── 0. 클라이언트(라즈베리) 연결 신호 수신 ───────────────────
        if (!client_connected && client_conn_fifo_fd >= 0)
        {
            char buf[64];
            int n = static_cast<int>(::read(client_conn_fifo_fd, buf, sizeof(buf) - 1));
            if (n > 0)
            {
                buf[n] = '\0';
                if (std::strstr(buf, "CONNECTED"))
                {
                    client_connected = true;
                    std::cerr << "[rtsp_laser_demo] 라즈베리 파이 연결됨 → LUT 시작\n";
                }
            }
        }

        bool lut_just_advanced = false;

        // ── 2. 타겟 ROI (좌표만 계산, 아직 프레임에 그리지 않음) ─────────
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

        // ── 3. 레이저 검출 (오버레이 그리기 전 원본 프레임에서 수행)
        //     초록 완료 셀을 먼저 그리면 해당 영역 픽셀이 가려져 레이저 탐지 실패함
        DetectionResult laser = detector.detectLaser(frame);
        bool used_manual_laser = false;
        if (g_auto_lut && g_manual_laser_pending)
        {
            laser.found = true;
            laser.point = cv::Point2f(static_cast<float>(g_manual_laser_pt.x),
                                      static_cast<float>(g_manual_laser_pt.y));
            used_manual_laser = true;
        }

        // ── 4. 타겟 ROI & LUT 그리드 오버레이 그리기 (레이저 검출 이후에 그려서
        //        완료 셀 초록색이 레이저 픽셀을 가리지 않음)
        if (targetROI.valid)
        {
            cv::rectangle(frame, targetROI.rect, cv::Scalar(0, 255, 0), 2);
            cv::circle(frame, targetROI.center(), 3, cv::Scalar(0, 255, 0), -1);
        }
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
                for (int i = 0; i < lut.idx; ++i)
                {
                    int r = i / LUT_GRID_COLS, c = i % LUT_GRID_COLS;
                    int x0 = c * W / LUT_GRID_COLS, x1 = (c+1) * W / LUT_GRID_COLS;
                    int y0 = r * H / LUT_GRID_ROWS, y1 = (r+1) * H / LUT_GRID_ROWS;
                    cv::rectangle(frame, cv::Rect(x0, y0, x1-x0, y1-y0),
                                  cv::Scalar(0, 80, 0), -1);
                }
                {
                    int r = lut.gr(), c = lut.gc();
                    int x0 = c * W / LUT_GRID_COLS, x1 = (c+1) * W / LUT_GRID_COLS;
                    int y0 = r * H / LUT_GRID_ROWS, y1 = (r+1) * H / LUT_GRID_ROWS;
                    cv::rectangle(frame, cv::Rect(x0, y0, x1-x0, y1-y0),
                                  cv::Scalar(0, 255, 255), 2);
                }
            }
        }
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
        if (g_auto_lut && used_manual_laser)
        {
            cv::drawMarker(frame, g_manual_laser_pt, cv::Scalar(255, 255, 0),
                           cv::MARKER_CROSS, 16, 2);
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

        // AutoLUT: (1) 수동 클릭이 있으면 즉시 PWM 요청/저장 (오차 전송은 이 프레임에서 생략)
        //         (2) 그 외에는 수렴(±6px, 3초) 시 PWM 요청/저장
        bool skip_periodic_send = false;
        if (g_auto_lut && client_connected && targetROI.valid && laser.found)
        {
            if (used_manual_laser)
            {
                lut_just_advanced = lut.requestPwmAndSave(target_u, target_v, std::cout);
                g_manual_laser_pending = false;
                skip_periodic_send = true;
            }
            else if (lut.updateConvergence(e_u, e_v, dt))
            {
                lut_just_advanced = lut.requestPwmAndSave(target_u, target_v, std::cout);
            }
        }

        // 전송: 다음 셀로 넘어간 직후에는 "다음 셀 중심 vs 현재 레이저" 오차를 반드시 1회 전송
        // 그 외에는 (타겟+레이저 있을 때) 주기 전송
        double send_e_u = e_u, send_e_v = e_v;
        double send_target_u = target_u, send_target_v = target_v;
        int send_gc = std::clamp(static_cast<int>(target_u * LUT_GRID_COLS / W), 0, LUT_GRID_COLS - 1);
        int send_gr = std::clamp(static_cast<int>(target_v * LUT_GRID_ROWS / H), 0, LUT_GRID_ROWS - 1);

        if (lut_just_advanced && g_auto_lut && !lut.done)
        {
            TargetROI newTarget = lut.currentTarget(W, H);
            send_target_u = static_cast<double>(newTarget.center().x);
            send_target_v = static_cast<double>(newTarget.center().y);
            send_gr = lut.gr();
            send_gc = lut.gc();
            if (laser.found)
            {
                send_e_u = send_target_u - static_cast<double>(laser.point.x);
                send_e_v = send_target_v - static_cast<double>(laser.point.y);
            }
            else
                send_e_u = send_e_v = 0.0;
        }

        bool should_send = (g_auto_lut && lut_just_advanced)
            || (!skip_periodic_send && client_connected && targetROI.valid && laser.found && (frame_id % SEND_EVERY_N == 0));

        if (should_send && targetROI.valid)
        {
            std::cout << send_e_u      << " "
                      << send_e_v      << " "
                      << send_target_u << " "
                      << send_target_v << " "
                      << send_gr       << " "
                      << send_gc       << std::endl;
            if (g_auto_lut && lut_just_advanced)
                std::cout.flush();
        }

        // ── 6. Auto LUT 오버레이 ───────────────────────────────────
        if (g_auto_lut && !lut.done)
            drawLutOverlay(frame, lut);

        // 레이저 포인터가 초록색 그리드/ROI/완료 셀에 가려지지 않도록
        // 모든 오버레이를 그린 뒤 한 번 더 맨 위 레이어에 그려준다.
        if (laser.found)
            cv::circle(frame, laser.point, 5, cv::Scalar(0, 0, 255), -1);

        cv::imshow("rtsp_laser_demo", frame);
        int key = cv::waitKey(1);
        if (key == 27 || key == 'q') break;
    }

    return 0;
}
