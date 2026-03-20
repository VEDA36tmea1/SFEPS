// camera_CSRT.cpp
// - RTSP + ONVIF 메타데이터로 객체 bbox를 얻고, 마우스 클릭으로 타겟 bbox를 선택한다.
// - 클릭한 bbox를 초기 ROI로 OpenCV CSRT 트래커를 시작하고, 이후 타겟 ROI를 CSRT가 추적한다.
// - 타겟이 확정되면 다른 객체(ONVIF) 박스는 숨기고 CSRT bbox만 표시한다.
// - CSRT bbox에서 (u,v) 타겟 포인트를 만들고, 기존 camera_RBF와 동일한 RBF(TPS)로 pan/tilt를 만든다.
// - stdout으로 "SET_PWM,PAN=...,TILT=..." 를 출력 (ubuntu_tcp_server 파이프 연동용)

#include "RTSPClient.h"
#include "XMLParser.h"
#include "Config.h"

#include <opencv2/opencv.hpp>
#include <opencv2/tracking.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_detect_all{false};

static cv::Rect expand_and_clamp_rect(const cv::Rect& r, double scale, int W, int H)
{
    if (W <= 0 || H <= 0) return cv::Rect(0, 0, 1, 1);
    cv::Rect rr = r;
    rr.width = std::max(1, rr.width);
    rr.height = std::max(1, rr.height);

    double cx = rr.x + rr.width * 0.5;
    double cy = rr.y + rr.height * 0.5;
    double nw = rr.width * scale;
    double nh = rr.height * scale;

    int x = (int)std::floor(cx - nw * 0.5);
    int y = (int)std::floor(cy - nh * 0.5);
    int w = (int)std::ceil(nw);
    int h = (int)std::ceil(nh);

    x = std::max(0, std::min(x, W - 1));
    y = std::max(0, std::min(y, H - 1));
    w = std::max(1, std::min(w, W - x));
    h = std::max(1, std::min(h, H - y));

    return cv::Rect(x, y, w, h);
}

static std::mutex g_obj_mutex;
static std::vector<ParsedMetadataObject> g_objects;

static cv::Mat g_last_frame;
static std::mutex g_frame_mutex;

static std::mutex g_sel_mutex;
static std::string g_selected_id;
static cv::Rect g_selected_rect;
static bool g_selected_valid = false;

static std::mutex g_pwm_mutex;
static int g_last_pan = 1500;
static int g_last_tilt = 1500;

static std::mutex g_click_mutex;
static bool g_click_pending = false;
static std::string g_click_pending_id;

static void signal_handler(int) { g_running = false; }

static bool compute_rect_from_obj(const ParsedMetadataObject& obj, int W, int H, cv::Rect& out)
{
    int left, right, top, bottom;
    if (std::max({obj.left, obj.right, obj.top, obj.bottom}) <= 1.5f)
    {
        left = static_cast<int>(obj.left * W);
        right = static_cast<int>(obj.right * W);
        top = static_cast<int>(obj.top * H);
        bottom = static_cast<int>(obj.bottom * H);
    }
    else
    {
        const double sx = static_cast<double>(W) / SENSOR_WIDTH;
        const double sy = static_cast<double>(H) / SENSOR_HEIGHT;
        left = static_cast<int>(obj.left * sx);
        right = static_cast<int>(obj.right * sx);
        top = static_cast<int>(obj.top * sy);
        bottom = static_cast<int>(obj.bottom * sy);
    }

    int width = std::max(1, right - left);
    int height = std::max(1, bottom - top);
    out = cv::Rect(left, top, width, height);

    // clamp
    out.x = std::max(0, std::min(out.x, W - 1));
    out.y = std::max(0, std::min(out.y, H - 1));
    out.width = std::max(1, std::min(out.width, W - out.x));
    out.height = std::max(1, std::min(out.height, H - out.y));
    return (out.area() > 20);
}

static void metadata_thread_fn(RTSPClient* client, XMLParser* parser)
{
    unsigned char header[4];
    char* big_buffer = new char[65536];
    std::string accumulated_xml;
    unsigned int last_timestamp = 0;
    int sock = client->getSocket();

    while (g_running)
    {
        client->sendHeartbeat();

        int read_len = recv(sock, header, 4, MSG_WAITALL);
        if (read_len <= 0) break;
        if (header[0] != '$') continue;

        int channel = (int)header[1];
        int payload_len = ((int)header[2] << 8) | (int)header[3];

        int total_read = 0;
        while (total_read < payload_len)
        {
            int to_read = payload_len - total_read;
            if (to_read > 65536) to_read = 65536;
            int r = recv(sock, big_buffer + total_read, to_read, 0);
            if (r <= 0)
            {
                total_read = 0;
                break;
            }
            total_read += r;
        }
        if (total_read <= 12) continue;

        if (channel == 2)
        {
            unsigned char* rtp_ptr = (unsigned char*)big_buffer;
            unsigned int current_timestamp =
                (rtp_ptr[4] << 24) | (rtp_ptr[5] << 16) | (rtp_ptr[6] << 8) | rtp_ptr[7];
            char* xml_data = big_buffer + 12;
            int xml_len = total_read - 12;

            if (current_timestamp != last_timestamp && last_timestamp != 0)
            {
                auto objs = parser->parseHumanObjectsForAnalytics(accumulated_xml, g_detect_all.load());
                {
                    std::lock_guard<std::mutex> lock(g_obj_mutex);
                    g_objects = std::move(objs);
                }
                accumulated_xml.clear();
            }
            accumulated_xml.append(xml_data, xml_len);
            last_timestamp = current_timestamp;
        }
    }

    delete[] big_buffer;
}

static void on_mouse(int event, int x, int y, int /*flags*/, void* userdata)
{
    if (event != cv::EVENT_LBUTTONDOWN) return;

    cv::Mat* frame_ptr = static_cast<cv::Mat*>(userdata);
    cv::Mat frame_copy;
    {
        std::lock_guard<std::mutex> lock(g_frame_mutex);
        if (frame_ptr->empty()) return;
        frame_copy = frame_ptr->clone();
    }
    int W = frame_copy.cols;
    int H = frame_copy.rows;

    std::vector<ParsedMetadataObject> objs;
    {
        std::lock_guard<std::mutex> lock(g_obj_mutex);
        objs = g_objects;
    }

    for (const auto& obj : objs)
    {
        cv::Rect rect;
        if (!compute_rect_from_obj(obj, W, H, rect)) continue;
        if (!rect.contains(cv::Point(x, y))) continue;

        {
            std::lock_guard<std::mutex> lock(g_sel_mutex);
            g_selected_id = obj.id;
            g_selected_rect = rect; // 클릭 순간 ROI를 "초기 CSRT ROI"로 쓴다.
            g_selected_valid = true;
        }

        {
            std::lock_guard<std::mutex> lk(g_click_mutex);
            g_click_pending = true;
            g_click_pending_id = obj.id;
        }
        break;
    }
}

// --- RBF (Thin-Plate Spline) interpolator for 2D -> scalar ---
class RbfTps2D
{
public:
    bool fit(const std::vector<cv::Point2d>& X, const std::vector<double>& Y)
    {
        const int N = (int)X.size();
        if (N < 4 || (int)Y.size() != N) return false;

        cv::Mat A = cv::Mat::zeros(N + 3, N + 3, CV_64F);
        cv::Mat b = cv::Mat::zeros(N + 3, 1, CV_64F);

        for (int i = 0; i < N; ++i)
        {
            b.at<double>(i, 0) = Y[i];
            for (int j = 0; j < N; ++j)
            {
                double dx = X[i].x - X[j].x;
                double dy = X[i].y - X[j].y;
                double r = std::sqrt(dx * dx + dy * dy);
                A.at<double>(i, j) = phi(r);
            }
            // P block
            A.at<double>(i, N + 0) = 1.0;
            A.at<double>(i, N + 1) = X[i].x;
            A.at<double>(i, N + 2) = X[i].y;
        }

        // P^T block
        for (int j = 0; j < N; ++j)
        {
            A.at<double>(N + 0, j) = 1.0;
            A.at<double>(N + 1, j) = X[j].x;
            A.at<double>(N + 2, j) = X[j].y;
        }

        cv::Mat x;
        bool ok = cv::solve(A, b, x, cv::DECOMP_SVD);
        if (!ok) return false;

        w_.assign(N, 0.0);
        for (int i = 0; i < N; ++i)
            w_[i] = x.at<double>(i, 0);
        a0_ = x.at<double>(N + 0, 0);
        a1_ = x.at<double>(N + 1, 0);
        a2_ = x.at<double>(N + 2, 0);
        X_ = X;
        return true;
    }

    double eval(double x, double y) const
    {
        const int N = (int)X_.size();
        double s = 0.0;
        for (int i = 0; i < N; ++i)
        {
            double dx = x - X_[i].x;
            double dy = y - X_[i].y;
            double r = std::sqrt(dx * dx + dy * dy);
            s += w_[i] * phi(r);
        }
        s += a0_ + a1_ * x + a2_ * y;
        return s;
    }

private:
    static double phi(double r)
    {
        const double eps = 1e-6;
        double rr = r * r;
        return rr * std::log(r + eps);
    }

    std::vector<cv::Point2d> X_;
    std::vector<double> w_;
    double a0_{0.0}, a1_{0.0}, a2_{0.0};
};

struct CalibPoint
{
    double X_cm{0.0}, Y_cm{0.0};
    double u{0.0}, v{0.0};
    double pan{0.0}, tilt{0.0};
    std::string name;
};

static std::vector<CalibPoint> load_calib_points()
{
    const double data[][6] = {
        {-370, 65, 80, 405, 890, 1320},    // p0?
        {-370, 201, 401, 285, 1055, 1390}, // p1
        {-370, 406, 775, 191, 1220, 1420}, // p2
        {-370, 586, 970, 154, 1305, 1435}, // p3
        {-370, 880, 1154, 129, 1385, 1450}, // p4
        {-370, 990, 1201, 124, 1410, 1455}, // p5
        {-508, 990, 1085, 108, 1355, 1460}, // p6
        {0, 221, 1607, 550, 1605, 1235}, // p7
        {0, 571, 1608, 292, 1600, 1385}, // p8
        {0, 891, 1588, 221, 1585, 1430}, // p9
        {-234, 0, 5, 632, 870, 1170}, // p10
        {-234, 250, 785, 300, 1220, 1360}, // p11
        {-234, 560, 1163, 193, 1390, 1420}, // p12
        {-234, 870, 1310, 159, 1455, 1440}, // p13
        {-234, 1000, 1342, 149, 1470, 1450}, // p14
        {0, 100, 1535, 898, 1570, 1065}, // p15 (new)
    };

    std::vector<CalibPoint> pts;
    const int N = (int)(sizeof(data) / sizeof(data[0]));
    pts.reserve(N);
    for (int i = 0; i < N; ++i)
    {
        CalibPoint p;
        p.X_cm = data[i][0];
        p.Y_cm = data[i][1];
        p.u = data[i][2];
        p.v = data[i][3];
        p.pan = data[i][4];
        p.tilt = data[i][5];
        p.name = "p" + std::to_string(i);
        pts.push_back(p);
    }
    return pts;
}

static void draw_world_grid(cv::Mat& frame, const RbfTps2D& rbf_u, const RbfTps2D& rbf_v,
                             const std::vector<CalibPoint>& pts)
{
    const int W = frame.cols;
    const int H = frame.rows;

    auto world_to_pixel = [&](double X_cm, double Y_cm) -> cv::Point {
        double u = rbf_u.eval(X_cm, Y_cm);
        double v = rbf_v.eval(X_cm, Y_cm);
        return cv::Point((int)std::lround(u), (int)std::lround(v));
    };

    // 범위는 calib points에서 추정
    double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
    for (const auto& p : pts)
    {
        minX = std::min(minX, p.X_cm);
        maxX = std::max(maxX, p.X_cm);
        minY = std::min(minY, p.Y_cm);
        maxY = std::max(maxY, p.Y_cm);
    }

    const double stepX = 100.0;
    const double stepY = 100.0;
    const int samples = 60;

    // X-constant lines
    for (double X = std::floor(minX / stepX) * stepX; X <= maxX + 1e-6; X += stepX)
    {
        std::vector<cv::Point> poly;
        poly.reserve(samples);
        for (int i = 0; i < samples; ++i)
        {
            double t = (double)i / (double)(samples - 1);
            double Y = minY + t * (maxY - minY);
            cv::Point p = world_to_pixel(X, Y);
            if (p.x < -2000 || p.x > W + 2000 || p.y < -2000 || p.y > H + 2000) continue;
            poly.push_back(p);
        }
        if (poly.size() >= 2) cv::polylines(frame, poly, false, cv::Scalar(80, 80, 220), 1, cv::LINE_AA);
    }

    // Y-constant lines
    for (double Y = std::floor(minY / stepY) * stepY; Y <= maxY + 1e-6; Y += stepY)
    {
        std::vector<cv::Point> poly;
        poly.reserve(samples);
        for (int i = 0; i < samples; ++i)
        {
            double t = (double)i / (double)(samples - 1);
            double X = minX + t * (maxX - minX);
            cv::Point p = world_to_pixel(X, Y);
            if (p.x < -2000 || p.x > W + 2000 || p.y < -2000 || p.y > H + 2000) continue;
            poly.push_back(p);
        }
        if (poly.size() >= 2) cv::polylines(frame, poly, false, cv::Scalar(80, 220, 80), 1, cv::LINE_AA);
    }

    // draw calib points
    for (const auto& p : pts)
    {
        cv::Point uv((int)std::lround(p.u), (int)std::lround(p.v));
        cv::circle(frame, uv, 4, cv::Scalar(0, 200, 255), -1, cv::LINE_AA);
        cv::putText(frame, p.name.c_str(), uv + cv::Point(6, -6), cv::FONT_HERSHEY_SIMPLEX, 0.4,
                    cv::Scalar(0, 200, 255), 1);
    }
}

int main(int argc, char** argv)
{
    // camera_RBF와 동일한 기본값
    double ratio = 0.3;
    double alpha = 0.5; // pan/tilt smoothing
    int pan_min = 500, pan_max = 2500;
    int tilt_min = 500, tilt_max = 2500;
    int send_every_n = 1;
    bool draw_grid = true;

    // tracker_mode: 1=CSRT, 2=KCF, 3=KCF+ROI crop
    int tracker_mode = 3;
    double crop_scale = 1.5;
    int resize_w = 0;
    int resize_h = 0; // 0이면 resize 비활성화

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--detect-all") g_detect_all = true;
        else if (arg == "--ratio" && i + 1 < argc) ratio = std::atof(argv[++i]);
        else if (arg == "--alpha" && i + 1 < argc) alpha = std::atof(argv[++i]);
        else if (arg == "--send-every" && i + 1 < argc) send_every_n = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--no-grid") draw_grid = false;
        else if (arg == "--tracker-mode" && i + 1 < argc)
            tracker_mode = std::max(1, std::min(3, std::atoi(argv[++i])));
        else if (arg == "--crop-scale" && i + 1 < argc)
            crop_scale = std::atof(argv[++i]);
        else if (arg == "--track-size" && i + 2 < argc)
        {
            resize_w = std::max(1, std::atoi(argv[++i]));
            resize_h = std::max(1, std::atoi(argv[++i]));
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGPIPE, signal_handler);

    // Calib points
    std::vector<CalibPoint> pts = load_calib_points();

    // RBF fit: pixel(u,v) -> PWM
    std::vector<cv::Point2d> px;
    std::vector<double> pan_y, tilt_y;
    px.reserve(pts.size());
    pan_y.reserve(pts.size());
    tilt_y.reserve(pts.size());
    for (const auto& p : pts)
    {
        px.emplace_back(p.u, p.v);
        pan_y.emplace_back(p.pan);
        tilt_y.emplace_back(p.tilt);
    }
    RbfTps2D rbf_pan, rbf_tilt;
    if (!rbf_pan.fit(px, pan_y) || !rbf_tilt.fit(px, tilt_y))
    {
        std::cerr << "[RBF] fit failed\n";
        return -1;
    }

    // RBF fit: world(X,Y, cm) -> pixel(u,v) (optional grid overlay)
    RbfTps2D rbf_u, rbf_v;
    {
        std::vector<cv::Point2d> wxy;
        std::vector<double> uu, vv;
        wxy.reserve(pts.size());
        uu.reserve(pts.size());
        vv.reserve(pts.size());
        for (const auto& p : pts)
        {
            wxy.emplace_back(p.X_cm, p.Y_cm);
            uu.emplace_back(p.u);
            vv.emplace_back(p.v);
        }
        if (!rbf_u.fit(wxy, uu) || !rbf_v.fit(wxy, vv))
            std::cerr << "[GRID] world->pixel RBF fit failed (grid disabled)\n";
    }

    // RTSP metadata
    RTSPClient client;
    XMLParser parser;
    if (!client.connectToCamera()) return -1;
    client.sendHandshake();
    std::thread meta_thread(metadata_thread_fn, &client, &parser);

    // Video
    cv::VideoCapture cap(RTSP_URL);
    if (!cap.isOpened())
    {
        std::cerr << "[camera_CSRT] RTSP open fail: " << RTSP_URL << "\n";
        g_running = false;
        meta_thread.join();
        return -1;
    }

    cv::namedWindow("camera_CSRT", cv::WINDOW_NORMAL);
    cv::setMouseCallback("camera_CSRT", on_mouse, &g_last_frame);

    int prev_pan = 1500, prev_tilt = 1500;
    int frame_id = 0;
    auto t_fps0 = std::chrono::steady_clock::now();

    // CSRT/KCF tracker state
    bool tracker_active = false;
    bool tracker_inited_for_id = false;
    std::string prev_sel_id;
    cv::Ptr<cv::Tracker> tracker;

    // tracker 좌표계(box_res) + 원본 좌표계(box_full) 변환용 배율
    bool track_resize = (resize_w > 0 && resize_h > 0);
    double sx = 1.0, sy = 1.0; // full -> track
    cv::Rect tracked_box_track; // tracker 좌표계 기준
    cv::Rect crop_rect_track;    // tracker 좌표계 기준

    bool use_kcf = (tracker_mode == 2 || tracker_mode == 3);
    bool do_crop = (tracker_mode == 3);

    while (g_running)
    {
        cv::Mat frame;
        if (!cap.read(frame) || frame.empty()) break;

        {
            std::lock_guard<std::mutex> lock(g_frame_mutex);
            g_last_frame = frame.clone();
        }

        const int W = frame.cols;
        const int H = frame.rows;

        // tracker용 프레임/좌표계 (옵션으로 downscale 가능)
        cv::Mat frame_track;
        int Wt = W;
        int Ht = H;
        // full -> track 변환배율: x_track = x_full * sx
        sx = 1.0;
        sy = 1.0;
        if (track_resize)
        {
            Wt = resize_w;
            Ht = resize_h;
            sx = (double)Wt / (double)W;
            sy = (double)Ht / (double)H;
            cv::resize(frame, frame_track, cv::Size(Wt, Ht));
        }
        else
        {
            frame_track = frame;
        }

        // copy objs for drawing + click selection
        std::vector<ParsedMetadataObject> objs;
        {
            std::lock_guard<std::mutex> lock(g_obj_mutex);
            objs = g_objects;
        }

        // selection
        std::string sel_id;
        cv::Rect sel_rect;
        bool sel_ok = false;
        {
            std::lock_guard<std::mutex> lock(g_sel_mutex);
            sel_ok = g_selected_valid;
            sel_id = g_selected_id;
            sel_rect = g_selected_rect;
        }

        // (1) tracker (재)초기화: sel_id가 바뀌면 클릭 ROI로 CSRT를 새로 init
        if (sel_ok && sel_id != prev_sel_id)
        {
            prev_sel_id = sel_id;
            tracker_inited_for_id = false;

            // 클릭 순간 bbox가 초기 ROI
            cv::Rect init_box_full = sel_rect;

            // init ROI를 tracker 좌표계로 변환
            cv::Rect init_box_track(
                (int)std::lround(init_box_full.x * sx),
                (int)std::lround(init_box_full.y * sy),
                (int)std::lround(init_box_full.width * sx),
                (int)std::lround(init_box_full.height * sy));

            init_box_track.x = std::max(0, std::min(init_box_track.x, Wt - 1));
            init_box_track.y = std::max(0, std::min(init_box_track.y, Ht - 1));
            init_box_track.width = std::max(1, std::min(init_box_track.width, Wt - init_box_track.x));
            init_box_track.height = std::max(1, std::min(init_box_track.height, Ht - init_box_track.y));

            if (use_kcf)
                tracker = cv::TrackerKCF::create();
            else
                tracker = cv::TrackerCSRT::create();

            try
            {
                tracker_active = false;
                tracker_inited_for_id = false;

                if (do_crop)
                {
                    crop_rect_track = expand_and_clamp_rect(init_box_track, crop_scale, Wt, Ht);
                    cv::Mat frame_crop = frame_track(crop_rect_track);

                    cv::Rect local_init(
                        init_box_track.x - crop_rect_track.x,
                        init_box_track.y - crop_rect_track.y,
                        init_box_track.width,
                        init_box_track.height);

                    local_init.x = std::max(0, std::min(local_init.x, crop_rect_track.width - 1));
                    local_init.y = std::max(0, std::min(local_init.y, crop_rect_track.height - 1));
                    local_init.width = std::max(1, std::min(local_init.width, crop_rect_track.width - local_init.x));
                    local_init.height = std::max(1, std::min(local_init.height, crop_rect_track.height - local_init.y));

                    tracker->init(frame_crop, local_init);
                    tracked_box_track = init_box_track;
                }
                else
                {
                    tracker->init(frame_track, init_box_track);
                    tracked_box_track = init_box_track;
                }

                tracker_active = true;
                tracker_inited_for_id = true;
            }
            catch (const cv::Exception&)
            {
                tracker_active = false;
                tracker_inited_for_id = false;
            }
        }

        // (2) draw onvif boxes / draw tracker bbox
        bool draw_onvif = !tracker_active; // tracker 활성화되면 다른 객체는 숨긴다.
        if (draw_onvif)
        {
            for (const auto& obj : objs)
            {
                cv::Rect r;
                if (!compute_rect_from_obj(obj, W, H, r)) continue;
                cv::rectangle(frame, r, cv::Scalar(0, 255, 255), 2);
                cv::putText(frame, obj.id.c_str(), cv::Point(r.x, std::max(0, r.y - 5)), cv::FONT_HERSHEY_SIMPLEX,
                            0.5, cv::Scalar(0, 255, 255), 1);
            }
        }

        // (3) CSRT update + target point + RBF -> PWM
        int target_u = W / 2;
        int target_v = H / 2;
        int pan = prev_pan;
        int tilt = prev_tilt;

        if (tracker_active && tracker_inited_for_id)
        {
            bool ok = false;

            if (do_crop)
            {
                crop_rect_track = expand_and_clamp_rect(tracked_box_track, crop_scale, Wt, Ht);
                cv::Mat frame_crop = frame_track(crop_rect_track);

                cv::Rect local_box(
                    tracked_box_track.x - crop_rect_track.x,
                    tracked_box_track.y - crop_rect_track.y,
                    tracked_box_track.width,
                    tracked_box_track.height);

                // local box clamp (crop 영역 밖으로 튀지 않게)
                local_box.x = std::max(0, std::min(local_box.x, crop_rect_track.width - 1));
                local_box.y = std::max(0, std::min(local_box.y, crop_rect_track.height - 1));
                local_box.width = std::max(1, std::min(local_box.width, crop_rect_track.width - local_box.x));
                local_box.height = std::max(1, std::min(local_box.height, crop_rect_track.height - local_box.y));

                ok = tracker->update(frame_crop, local_box);

                tracked_box_track = cv::Rect(
                    local_box.x + crop_rect_track.x,
                    local_box.y + crop_rect_track.y,
                    local_box.width,
                    local_box.height);
            }
            else
            {
                ok = tracker->update(frame_track, tracked_box_track);
            }

            if (!ok)
            {
                // 트래커가 실패하면 다시 온비프 bbox 모드로 되돌린다.
                tracker_active = false;
                tracker_inited_for_id = false;
            }
        }

        if (tracker_active && tracker_inited_for_id)
        {
            // draw tracked bbox
            // tracked_box_track 은 track(예: 640x360) 좌표계 기준이다.
            // 여기서는 원본(full) 좌표계로 변환해서 RBF/그리기를 수행한다.
            double inv_sx = (sx != 0.0) ? (1.0 / sx) : 1.0;
            double inv_sy = (sy != 0.0) ? (1.0 / sy) : 1.0;

            cv::Rect track_rect_full(
                (int)std::lround(tracked_box_track.x * inv_sx),
                (int)std::lround(tracked_box_track.y * inv_sy),
                (int)std::lround(tracked_box_track.width * inv_sx),
                (int)std::lround(tracked_box_track.height * inv_sy));

            track_rect_full.x = std::max(0, std::min(track_rect_full.x, W - 1));
            track_rect_full.y = std::max(0, std::min(track_rect_full.y, H - 1));
            track_rect_full.width = std::max(1, std::min(track_rect_full.width, W - track_rect_full.x));
            track_rect_full.height = std::max(1, std::min(track_rect_full.height, H - track_rect_full.y));

            cv::rectangle(frame, track_rect_full, cv::Scalar(0, 255, 0), 2);
            std::string tracker_name = use_kcf ? "KCF " : "CSRT ";
            cv::putText(frame, (tracker_name + sel_id).c_str(),
                        cv::Point(track_rect_full.x, std::max(0, track_rect_full.y - 10)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);

            // target point from bbox (ratio, full coords 기준)
            target_u = track_rect_full.x + track_rect_full.width / 2;
            target_v = track_rect_full.y + (int)std::lround(track_rect_full.height * ratio);
            target_v = std::max(0, std::min(H - 1, target_v));

            cv::line(frame, cv::Point(track_rect_full.x, target_v),
                     cv::Point(track_rect_full.x + track_rect_full.width, target_v),
                     cv::Scalar(0, 255, 0), 1);

            double pan_d = rbf_pan.eval((double)target_u, (double)target_v);
            double tilt_d = rbf_tilt.eval((double)target_u, (double)target_v);

            pan = (int)std::lround(std::max((double)pan_min, std::min((double)pan_max, pan_d)));
            tilt = (int)std::lround(std::max((double)tilt_min, std::min((double)tilt_max, tilt_d)));

            // smoothing
            pan = (int)std::lround(alpha * pan + (1.0 - alpha) * prev_pan);
            tilt = (int)std::lround(alpha * tilt + (1.0 - alpha) * prev_tilt);
            prev_pan = pan;
            prev_tilt = tilt;

            {
                std::lock_guard<std::mutex> lk(g_pwm_mutex);
                g_last_pan = pan;
                g_last_tilt = tilt;
            }

            cv::circle(frame, cv::Point(target_u, target_v), 8, cv::Scalar(0, 165, 255), -1, cv::LINE_AA);
            cv::circle(frame, cv::Point(target_u, target_v), 8, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
        }

        // stdout -> ubuntu_tcp_server (pipe)
        if (tracker_active && frame_id % send_every_n == 0)
        {
            std::cout << "SET_PWM,PAN=" << pan << ",TILT=" << tilt << std::endl;
        }
        else if (frame_id % 30 == 0)
        {
            // 선택 해제/트래커 실패 시에도 pipe health 체크용 heartbeat
            std::cout << std::endl;
        }

        if (!std::cout)
        {
            g_running = false;
            break;
        }

        // 클릭 직후 디버그(참고용)
        {
            std::lock_guard<std::mutex> lk(g_click_mutex);
            if (g_click_pending && sel_ok && sel_id == g_click_pending_id)
            {
                std::cerr << "CLICK_INIT "
                          << "id=" << sel_id
                          << " init_rect=(" << sel_rect.x << "," << sel_rect.y << "," << sel_rect.width << "," << sel_rect.height << ")"
                          << " target=(" << target_u << "," << target_v << ")"
                          << " PAN=" << pan << " TILT=" << tilt << std::endl;
                g_click_pending = false;
                g_click_pending_id.clear();
            }
        }

        // grid overlay
        if (draw_grid) draw_world_grid(frame, rbf_u, rbf_v, pts);

        // info + fps
        char info[256];
        std::string tracker_name = use_kcf ? "KCF" : "CSRT";
        std::snprintf(info, sizeof(info), "%s active=%d crop=%d ratio=%.3f pan=%d tilt=%d",
                      tracker_name.c_str(), tracker_active ? 1 : 0, do_crop ? 1 : 0, ratio, pan, tilt);
        cv::putText(frame, info, cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 255), 2);

        frame_id++;
        if (frame_id % 30 == 0)
        {
            auto t1 = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(t1 - t_fps0).count();
            double fps = (dt > 1e-6) ? (30.0 / dt) : 0.0;
            t_fps0 = t1;
            std::cerr << "[FPS] " << fps << "\n";
        }

        cv::imshow("camera_CSRT", frame);
        int key = cv::waitKey(1) & 0xFF;
        if (key == 27 || key == 'q')
        {
            g_running = false;
            break;
        }
    }

    g_running = false;
    if (meta_thread.joinable()) meta_thread.join();
    cap.release();
    cv::destroyAllWindows();
    return 0;
}

