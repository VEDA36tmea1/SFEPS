#include "vision_detector.h"
#include "target_provider.h"
#include "ibvs_controller.h"
#include "gst_ibvs.h"

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <glib.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
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
static constexpr int   SEND_EVERY_N = 1;    // 파이프 전송 주기(프레임)
static constexpr float SETTLE_THRESH_PX = 6.0f; // 수렴 판정 픽셀 오차
static constexpr float SETTLE_TIME_SEC  = 3.0f; // 수렴 유지 시간(초)

static bool g_lut_mode  = true;
static bool g_auto_lut  = false;  // --lut-auto
static bool g_lut_track = false;  // --lut-track : LUT 기반 추종 모드
static bool g_lut_check = false;  // --lut-check : LUT JSON 기반 PWM 검증 모드

// 레이저/HSV 튜닝용 픽셀 프로브 모드(L 키 토글)
static bool g_probe_mode  = false;
static bool g_probe_valid = false;
static cv::Point g_probe_pt(0, 0);

// D 키: 스테이지 디버그 — R-dominance 마스크(빨간색만 남긴 영상) 창 표시
static bool g_stage_debug = false;

// ─── World-track: 바운딩 박스 중심 (u,v) → Homography → (X,Y) mm → pan/tilt 각도 → SET_PWM ─
static bool g_world_track = false;
static cv::Mat g_H_world;  // 3x3, pixel -> world (mm); empty if not loaded
static double g_laser_x_mm = 840.0;
static double g_laser_y_mm = -3070.0;
static double g_laser_z_mm = 2230.0;
static double g_target_z_mm = 1200.0;   // 사람 가슴 높이 가정 (mm)
static double g_pan_center_us = 1530.0;
static double g_tilt_center_us = 1300.0;
static double g_pan_us_per_deg = 10.0;   // 1도당 µs (캘리브 후 조정)
static double g_tilt_us_per_deg = 10.0;

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

// 레이저 체크(픽셀 프로브)용 마우스 콜백
static void onMouseProbe(int event, int x, int y, int /*flags*/, void* /*userdata*/)
{
    if (!g_probe_mode)
        return;
    if (event == cv::EVENT_LBUTTONDOWN)
    {
        g_probe_pt = cv::Point(x, y);
        g_probe_valid = true;
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

struct LutPoint
{
    int grid_r{-1};
    int grid_c{-1};
    double target_u{0.0};
    double target_v{0.0};
    double pan_us{0.0};
    double tilt_us{0.0};
};

static bool load_lut_points(const std::string& path, std::vector<LutPoint>& out, int& rows, int& cols)
{
    std::ifstream ifs(path);
    if (!ifs) return false;
    std::string s((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    // rows/cols
    rows = cols = -1;
    {
        size_t pr = s.find("\"rows\"");
        size_t pc = s.find("\"cols\"");
        if (pr != std::string::npos) std::sscanf(s.c_str() + pr, "\"rows\":%d", &rows);
        if (pc != std::string::npos) std::sscanf(s.c_str() + pc, "\"cols\":%d", &cols);
    }

    out.clear();
    size_t pos = 0;
    while (true)
    {
        size_t b = s.find("{\"grid_r\"", pos);
        if (b == std::string::npos) break;
        size_t e = s.find("}", b);
        if (e == std::string::npos) break;
        std::string obj = s.substr(b, e - b + 1);

        LutPoint p;
        // {"grid_r":0,"grid_c":0,"target_u":50.0,"target_v":49.0,"pan_us":1019,"tilt_us":1636}
        if (std::sscanf(obj.c_str(),
                        "{\"grid_r\":%d,\"grid_c\":%d,\"target_u\":%lf,\"target_v\":%lf,\"pan_us\":%lf,\"tilt_us\":%lf",
                        &p.grid_r, &p.grid_c, &p.target_u, &p.target_v, &p.pan_us, &p.tilt_us) >= 6)
        {
            out.push_back(p);
        }
        pos = e + 1;
    }
    return !out.empty();
}

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
// World-track: Homography 로드, 픽셀→월드(mm), 월드→pan/tilt(deg)
// ══════════════════════════════════════════════════════════════════════════
static bool load_homography_yaml(const std::string& path, cv::Mat& H_out)
{
    cv::FileStorage fs(path, cv::FileStorage::READ);
    if (!fs.isOpened())
        return false;
    fs["H"] >> H_out;
    fs.release();
    if (H_out.empty() || H_out.rows != 3 || H_out.cols != 3)
        return false;
    return true;
}

// 픽셀 (u,v) → Homography 적용 → 평면 (X_mm, Y_mm). Z는 별도 지정.
static void pixel_to_world_mm(const cv::Mat& H, double u, double v, double& x_mm, double& y_mm)
{
    if (H.empty())
    {
        x_mm = y_mm = 0.0;
        return;
    }
    double w = H.at<double>(2, 0) * u + H.at<double>(2, 1) * v + H.at<double>(2, 2);
    if (std::fabs(w) < 1e-9)
        w = 1e-9;
    x_mm = (H.at<double>(0, 0) * u + H.at<double>(0, 1) * v + H.at<double>(0, 2)) / w;
    y_mm = (H.at<double>(1, 0) * u + H.at<double>(1, 1) * v + H.at<double>(1, 2)) / w;
}

// 레이저 원점 (g_laser_*_mm), 타겟 3D (x_mm, y_mm, z_mm) → pan(yaw), tilt(pitch) [deg]
static void world_to_pan_tilt_deg(
    double laser_x, double laser_y, double laser_z,
    double target_x_mm, double target_y_mm, double target_z_mm,
    double& pan_deg, double& tilt_deg)
{
    double dx = target_x_mm - laser_x;
    double dy = target_y_mm - laser_y;
    double dz = target_z_mm - laser_z;
    double horiz = std::sqrt(dx * dx + dy * dy);
    pan_deg = std::atan2(dx, dy) * 180.0 / M_PI;   // yaw: 좌우
    tilt_deg = std::atan2(dz, horiz) * 180.0 / M_PI; // pitch: 위/아래
}

// ══════════════════════════════════════════════════════════════════════════
// main
// ══════════════════════════════════════════════════════════════════════════
int main(int argc, char** argv)
{
    std::string uri = "rtsp://admin:CCgbdCCgbd@192.168.0.84/profile2/media.smp";
    bool use_gst = false;
    bool use_gst_launch = false;
    std::string gst_launch_pipeline;
    std::string lut_check_path = "lut_data.json";
    std::string homography_path = "homography.yml";

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if      (arg == "--nolut")       g_lut_mode  = false;
        else if (arg == "--lut-auto")   g_auto_lut  = true;
        else if (arg == "--lut-track")  g_lut_track = true;
        else if (arg == "--world-track")
        {
            g_world_track = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                homography_path = argv[++i];
        }
        else if (arg == "--homography" && i + 1 < argc)
            homography_path = argv[++i];
        else if (arg == "--laser-x" && i + 1 < argc)
            g_laser_x_mm = std::atof(argv[++i]);
        else if (arg == "--laser-y" && i + 1 < argc)
            g_laser_y_mm = std::atof(argv[++i]);
        else if (arg == "--laser-z" && i + 1 < argc)
            g_laser_z_mm = std::atof(argv[++i]);
        else if (arg == "--target-z-mm" && i + 1 < argc)
            g_target_z_mm = std::atof(argv[++i]);
        else if (arg == "--pan-center-us" && i + 1 < argc)
            g_pan_center_us = std::atof(argv[++i]);
        else if (arg == "--tilt-center-us" && i + 1 < argc)
            g_tilt_center_us = std::atof(argv[++i]);
        else if (arg == "--pan-us-per-deg" && i + 1 < argc)
            g_pan_us_per_deg = std::atof(argv[++i]);
        else if (arg == "--tilt-us-per-deg" && i + 1 < argc)
            g_tilt_us_per_deg = std::atof(argv[++i]);
        else if (arg == "--lut-check")
        {
            g_lut_check = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                lut_check_path = argv[++i];
        }
        else if (arg == "--gst")         use_gst     = true;
        else if (arg == "--gst-launch" && i + 1 < argc)
        {
            use_gst_launch = true;
            gst_launch_pipeline = argv[++i];
        }
        else
            uri = arg;
    }

    std::cerr << "[rtsp_laser_demo] open: " << uri << "\n";
    std::cerr << "[rtsp_laser_demo] LUT=" << (g_lut_mode ? "ON" : "OFF")
              << "  자동순회=" << (g_auto_lut ? "ON" : "OFF")
              << "  LUT-추종=" << (g_lut_track ? "ON" : "OFF")
              << "  World-track=" << (g_world_track ? "ON" : "OFF")
              << "  GStreamer=" << (use_gst ? "ON" : "OFF")
              << "  GstLaunch=" << (use_gst_launch ? "ON" : "OFF") << "\n";

    GstElement* gst_pipeline = nullptr;
    GstElement* gst_appsink_el = nullptr;

    if (use_gst_launch)
    {
        gst_init(&argc, &argv);
        GError* err = nullptr;
        gst_pipeline = gst_parse_launch(gst_launch_pipeline.c_str(), &err);
        if (err || !gst_pipeline)
        {
            std::cerr << "[rtsp_laser_demo] GStreamer pipeline 파싱 실패: "
                      << (err ? err->message : "unknown") << "\n";
            if (err) g_error_free(err);
            return 1;
        }
        gst_appsink_el = gst_bin_get_by_name(GST_BIN(gst_pipeline), "sink");
        if (!gst_appsink_el || !GST_IS_APP_SINK(gst_appsink_el))
        {
            std::cerr << "[rtsp_laser_demo] 파이프라인에 'appsink name=sink' 가 필요합니다.\n";
            if (gst_appsink_el) gst_object_unref(gst_appsink_el);
            gst_object_unref(gst_pipeline);
            return 1;
        }
        gst_element_set_state(gst_pipeline, GST_STATE_PLAYING);
        std::cerr << "[rtsp_laser_demo] GStreamer pipeline 시작 (appsink에서 GstSample 수신)\n";
    }

    cv::VideoCapture cap;
    if (!use_gst_launch && use_gst)
    {
        // GStreamer 파이프라인 (지연 최소화용, appsink로 프레임 수신)
        // 예시:
        // rtspsrc location=<uri> protocols=udp latency=100 drop-on-latency=false !
        //   rtph264depay ! h264parse ! avdec_h264 !
        //   videoconvert ! video/x-raw,format=BGR !
        //   appsink drop=true max-buffers=1 sync=false
        std::ostringstream oss;
        const int GST_LATENCY_MS = 0;
        oss << "rtspsrc location=" << uri
            << " protocols=udp latency=" << GST_LATENCY_MS
            << " drop-on-latency=true ! "
            << "rtph264depay ! h264parse ! avdec_h264 ! "
            << "videoconvert ! video/x-raw,format=BGR ! "
            << "appsink drop=true max-buffers=1 leaky=downstream sync=false";
        std::string pipeline = oss.str();

        std::cerr << "[rtsp_laser_demo] use GStreamer pipeline:\n  " << pipeline << "\n";
        cap.open(pipeline, cv::CAP_GSTREAMER);
    }
    else if (!use_gst_launch)
    {
        cap.open(uri);
    }

    if (!use_gst_launch && !cap.isOpened())
    {
        std::cerr << "[rtsp_laser_demo] RTSP 열기 실패 ("
                  << (use_gst ? "GStreamer" : "OpenCV") << ")\n";
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
    else
        cv::setMouseCallback("rtsp_laser_demo", onMouseProbe, nullptr);

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
    if (g_lut_track)
        std::cerr << "[rtsp_laser_demo] LUT-기반 추종 모드: 박스 중심(TU,TV)을 라즈베리로 전송 (레이저 탐지/오차 계산 없음)\n";
    if (g_lut_check)
        std::cerr << "[rtsp_laser_demo] LUT 체크 모드: lut_data.json의 PWM을 라즈베리에 SET_PWM으로 보내고, 레이저가 해당 셀로 오는지 확인\n";
    if (g_world_track)
    {
        if (load_homography_yaml(homography_path, g_H_world))
            std::cerr << "[rtsp_laser_demo] World-track: Homography 로드 성공 " << homography_path
                      << " | 레이저(" << g_laser_x_mm << "," << g_laser_y_mm << "," << g_laser_z_mm
                      << ") target_z=" << g_target_z_mm << " mm\n";
        else
        {
            std::cerr << "[rtsp_laser_demo] World-track: Homography 로드 실패 " << homography_path << " → 비활성화\n";
            g_world_track = false;
        }
    }
    if (!g_auto_lut && !g_lut_track && !g_world_track)
        std::cerr << "[rtsp_laser_demo] L 키: 픽셀 프로브 모드 토글 (좌클릭 시 RGB/HSV 출력)\n";

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
        if (use_gst_launch)
        {
            for (int k = 0; k < 10; ++k)
                g_main_context_iteration(g_main_context_default(), FALSE);
            GstSample* sample = gst_app_sink_try_pull_sample(
                GST_APP_SINK(gst_appsink_el), 100 * GST_MSECOND);
            if (!sample)
                continue;
            if (!gst_sample_to_mat(sample, frame) || frame.empty())
            {
                gst_sample_unref(sample);
                continue;
            }
            gst_sample_unref(sample);
        }
        else
        {
            if (!cap.read(frame) || frame.empty())
            {
                std::cerr << "[rtsp_laser_demo] empty frame\n";
                break;
            }
        }

        // 순수 영상 처리 시간 측정을 위한 시작 시각(프레임 획득 직후)
        auto proc_t0 = std::chrono::steady_clock::now();

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
        std::vector<LutPoint> lut_points;
        static bool lut_loaded = false;
        static size_t lut_check_idx = 0;
        static int lut_rows = LUT_GRID_ROWS;
        static int lut_cols = LUT_GRID_COLS;
        if (g_lut_check && !lut_loaded)
        {
            if (!load_lut_points(lut_check_path, lut_points, lut_rows, lut_cols))
            {
                std::cerr << "[LUT-check] LUT 파일 로드 실패: " << lut_check_path << "\n";
                break;
            }
            lut_loaded = true;
            std::cerr << "[LUT-check] loaded " << lut_points.size() << " points from " << lut_check_path << "\n";
        }

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

        // LUT 체크 모드: LUT 포인트의 목표 셀을 화면에 표시하고, 해당 PWM을 라즈베리에 전송
        if (g_lut_check)
        {
            // 레이저 검출 (그림 전)
            cv::Mat red_mask_debug;
            DetectionResult laser = detector.detectLaser(frame, g_stage_debug ? &red_mask_debug : nullptr);
            if (g_stage_debug && !red_mask_debug.empty())
                cv::imshow("red_mask (D=debug)", red_mask_debug);

            // grid overlay
            for (int c = 1; c < LUT_GRID_COLS; ++c)
            {
                int x = c * W / LUT_GRID_COLS;
                cv::line(frame, cv::Point(x, 0), cv::Point(x, H), cv::Scalar(60, 60, 60), 1);
            }
            for (int r = 1; r < LUT_GRID_ROWS; ++r)
            {
                int y = r * H / LUT_GRID_ROWS;
                cv::line(frame, cv::Point(0, y), cv::Point(W, y), cv::Scalar(60, 60, 60), 1);
            }

            // 현재 LUT point 선택
            if (lut_loaded)
            {
                // lut_points는 위에서 로드했지만 지역변수라 1프레임마다 사라짐 → 간단히 다시 로드 대신 static 보관
            }

            static std::vector<LutPoint> s_lut_points;
            if (g_lut_check && lut_loaded && s_lut_points.empty())
            {
                (void)load_lut_points(lut_check_path, s_lut_points, lut_rows, lut_cols);
            }
            if (s_lut_points.empty())
            {
                cv::imshow("rtsp_laser_demo", frame);
                int key = cv::waitKey(1);
                if (key == 27 || key == 'q') break;
                continue;
            }
            if (lut_check_idx >= s_lut_points.size())
                lut_check_idx = 0;
            const LutPoint& p = s_lut_points[lut_check_idx];

            // 목표 셀 표시
            int x0 = p.grid_c * W / LUT_GRID_COLS;
            int x1 = (p.grid_c + 1) * W / LUT_GRID_COLS;
            int y0 = p.grid_r * H / LUT_GRID_ROWS;
            int y1 = (p.grid_r + 1) * H / LUT_GRID_ROWS;
            cv::rectangle(frame, cv::Rect(x0, y0, x1 - x0, y1 - y0), cv::Scalar(0, 255, 255), 2);

            // 라즈베리에 PWM 전송(한 번만)
            static int last_sent_gr = -1, last_sent_gc = -1;
            if (client_connected && (p.grid_r != last_sent_gr || p.grid_c != last_sent_gc))
            {
                std::cout << "SET_PWM,PAN=" << std::fixed << std::setprecision(0) << p.pan_us
                          << ",TILT=" << p.tilt_us
                          << ",GR=" << p.grid_r << ",GC=" << p.grid_c
                          << std::endl;
                std::cout.flush();
                last_sent_gr = p.grid_r;
                last_sent_gc = p.grid_c;
                std::cerr << "[LUT-check] sent SET_PWM GR=" << p.grid_r << " GC=" << p.grid_c
                          << " pan=" << p.pan_us << " tilt=" << p.tilt_us << "\n";
            }

            // 레이저가 검출되면 현재 레이저 셀을 계산해서 맞으면 초록 표시
            if (laser.found)
            {
                int lc = std::clamp(static_cast<int>(laser.point.x * LUT_GRID_COLS / W), 0, LUT_GRID_COLS - 1);
                int lr = std::clamp(static_cast<int>(laser.point.y * LUT_GRID_ROWS / H), 0, LUT_GRID_ROWS - 1);
                if (lr == p.grid_r && lc == p.grid_c)
                {
                    cv::rectangle(frame, cv::Rect(x0, y0, x1 - x0, y1 - y0), cv::Scalar(0, 255, 0), 2);
                }
                cv::circle(frame, laser.point, 5, cv::Scalar(0, 0, 255), -1);
            }

            // 안내 텍스트
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "LUT-check idx=%zu/%zu  GR=%d GC=%d  pan=%.0f tilt=%.0f  (n=next, p=prev, q=quit)",
                              lut_check_idx + 1, s_lut_points.size(), p.grid_r, p.grid_c, p.pan_us, p.tilt_us);
                cv::putText(frame, buf, cv::Point(10, 30),
                            cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 255), 2);
            }

            cv::imshow("rtsp_laser_demo", frame);
            int key = cv::waitKey(1);
            if (key == 'n')
            {
                ++lut_check_idx;
            }
            else if (key == 'p')
            {
                if (lut_check_idx > 0) --lut_check_idx;
            }
            if (key == 'D' || key == 'd')
            {
                g_stage_debug = !g_stage_debug;
                if (!g_stage_debug)
                    cv::destroyWindow("red_mask (D=debug)");
            }
            if (key == 27 || key == 'q') break;
            continue;
        }

        // LUT 기반 추종 모드: 레이저 검출/오차 계산 없이, 박스 중심 픽셀 좌표만 전송
        if (g_lut_track)
        {
            if (targetROI.valid)
            {
                // ROI/그리드 오버레이
                cv::rectangle(frame, targetROI.rect, cv::Scalar(0, 255, 0), 2);
                cv::circle(frame, targetROI.center(), 3, cv::Scalar(0, 255, 0), -1);

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
                }

                double target_u = static_cast<double>(targetROI.center().x);
                double target_v = static_cast<double>(targetROI.center().y);
                int send_gc = std::clamp(static_cast<int>(target_u * LUT_GRID_COLS / W), 0, LUT_GRID_COLS - 1);
                int send_gr = std::clamp(static_cast<int>(target_v * LUT_GRID_ROWS / H), 0, LUT_GRID_ROWS - 1);

                if (client_connected && (frame_id % SEND_EVERY_N == 0))
                {
                    // EX/EY는 LUT 모드에서 무시하고 TU/TV/GR/GC만 사용
                    std::cout << 0.0              << " "
                              << 0.0              << " "
                              << target_u         << " "
                              << target_v         << " "
                              << send_gr          << " "
                              << send_gc          << std::endl;
                }
            }

            cv::imshow("rtsp_laser_demo", frame);
            int key = cv::waitKey(1);
            if (key == 'L' || key == 'l')
            {
                g_probe_mode = !g_probe_mode;
                g_probe_valid = false;
                std::cerr << "[rtsp_laser_demo] Probe mode " << (g_probe_mode ? "ON" : "OFF") << "\n";
            }
            if (key == 'D' || key == 'd')
            {
                g_stage_debug = !g_stage_debug;
                if (!g_stage_debug)
                    cv::destroyWindow("red_mask (D=debug)");
                std::cerr << "[rtsp_laser_demo] Stage debug (red mask) " << (g_stage_debug ? "ON" : "OFF") << "\n";
            }
            if (key == 27 || key == 'q') break;
            continue;
        }

        // ── 3. 레이저 검출 (오버레이 그리기 전 원본 프레임에서 수행)
        //     초록 완료 셀을 먼저 그리면 해당 영역 픽셀이 가려져 레이저 탐지 실패함
        cv::Mat red_mask_debug;
        DetectionResult laser = detector.detectLaser(frame, g_stage_debug ? &red_mask_debug : nullptr);
        if (g_stage_debug && !red_mask_debug.empty())
            cv::imshow("red_mask (D=debug)", red_mask_debug);
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

        // ── 5-1. 픽셀 프로브(RGB/HSV) 오버레이 ─────────────────────
        if (g_probe_mode && g_probe_valid)
        {
            int px = g_probe_pt.x;
            int py = g_probe_pt.y;
            if (0 <= px && px < frame.cols && 0 <= py && py < frame.rows)
            {
                cv::Vec3b bgr = frame.at<cv::Vec3b>(py, px);
                cv::Mat hsv;
                cv::cvtColor(frame, hsv, cv::COLOR_BGR2HSV);
                cv::Vec3b hsvv = hsv.at<cv::Vec3b>(py, px);

                int B = bgr[0];
                int G = bgr[1];
                int R = bgr[2];
                int H = hsvv[0];
                int S = hsvv[1];
                int V = hsvv[2];

                std::fprintf(stderr,
                             "[probe] P=(%d,%d) RGB=(%d,%d,%d) HSV=(%d,%d,%d)\n",
                             px, py, R, G, B, H, S, V);

                cv::drawMarker(frame, g_probe_pt, cv::Scalar(0, 255, 255),
                               cv::MARKER_CROSS, 18, 2);

                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              "P=(%d,%d) R=%d G=%d B=%d H=%d S=%d V=%d",
                              px, py, R, G, B, H, S, V);
                cv::putText(frame, buf, cv::Point(10, frame.rows - 20),
                            cv::FONT_HERSHEY_SIMPLEX, 0.5,
                            cv::Scalar(0, 255, 255), 1);
            }
            g_probe_valid = false; // 한 번 클릭 당 한 번만 로그
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
        // --nolut 모드에서 Laser_Detection_Delay.py 등이 값을 받을 수 있도록, 레이저만 감지돼도 전송 (박스 불필요)
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

        // nolut/non-LUT 모드: 레이저만 감지돼도 전송 (Laser_Detection_Delay.py 지연 측정용)
        bool nolut_send = (!g_auto_lut && !g_lut_track && client_connected && laser.found && (frame_id % SEND_EVERY_N == 0));
        if (nolut_send && !targetROI.valid)
        {
            send_e_u = send_e_v = 0.0;
            send_target_u = static_cast<double>(laser.point.x);
            send_target_v = static_cast<double>(laser.point.y);
            send_gr = send_gc = 0;
        }

        // World-track: 바운딩 박스 중심 (u,v) → Homography → (X,Y) mm → pan/tilt deg → PWM → SET_PWM
        if (g_world_track && client_connected && targetROI.valid && !g_H_world.empty() && (frame_id % SEND_EVERY_N == 0))
        {
            double x_mm = 0.0, y_mm = 0.0;
            pixel_to_world_mm(g_H_world, target_u, target_v, x_mm, y_mm);
            double pan_deg = 0.0, tilt_deg = 0.0;
            world_to_pan_tilt_deg(
                g_laser_x_mm, g_laser_y_mm, g_laser_z_mm,
                x_mm, y_mm, g_target_z_mm,
                pan_deg, tilt_deg);
            double pan_us  = g_pan_center_us  + pan_deg  * g_pan_us_per_deg;
            double tilt_us = g_tilt_center_us + tilt_deg * g_tilt_us_per_deg;
            pan_us  = std::max(800.0, std::min(2200.0, pan_us));
            tilt_us = std::max(800.0, std::min(2200.0, tilt_us));
            std::cout << "SET_PWM,PAN=" << std::fixed << std::setprecision(0) << pan_us
                      << ",TILT=" << tilt_us << std::endl;
            std::cout.flush();
            if (frame_id % 30 == 0)
                std::cerr << "[world-track] (" << target_u << "," << target_v << ") → ("
                          << x_mm << "," << y_mm << ") mm → pan=" << pan_deg << "° tilt=" << tilt_deg
                          << "° → " << pan_us << "," << tilt_us << " us\n";
        }

        bool should_send = (g_auto_lut && lut_just_advanced)
            || (!skip_periodic_send && client_connected && laser.found && (frame_id % SEND_EVERY_N == 0)
                && (targetROI.valid || (!g_auto_lut && !g_lut_track)));

        if (should_send)
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

        // 순수 영상 처리 시간 측정: 프레임 획득 이후 ~ 오버레이 완료 직전
        auto   proc_t1 = std::chrono::steady_clock::now();
        double proc_ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(proc_t1 - proc_t0).count();
        if (frame_id % 30 == 0)  // 30프레임마다 한 번씩만 로그
        {
            std::cerr << "[perf] vision proc = " << proc_ms << " ms/frame\n";
        }

        cv::imshow("rtsp_laser_demo", frame);
        int key = cv::waitKey(1);
        if (key == 'L' || key == 'l')
        {
            g_probe_mode = !g_probe_mode;
            g_probe_valid = false;
            std::cerr << "[rtsp_laser_demo] Probe mode " << (g_probe_mode ? "ON" : "OFF") << "\n";
        }
        if (key == 'D' || key == 'd')
        {
            g_stage_debug = !g_stage_debug;
            if (!g_stage_debug)
                cv::destroyWindow("red_mask (D=debug)");
            std::cerr << "[rtsp_laser_demo] Stage debug (red mask) " << (g_stage_debug ? "ON" : "OFF") << "\n";
        }
        if (key == 27 || key == 'q') break;
    }

    if (use_gst_launch && gst_pipeline)
    {
        gst_element_set_state(gst_pipeline, GST_STATE_NULL);
        if (gst_appsink_el) gst_object_unref(gst_appsink_el);
        gst_object_unref(gst_pipeline);
    }

    return 0;
}
