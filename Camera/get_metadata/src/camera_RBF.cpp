// camera_RBF.cpp
// - camera_client.cpp 기반: ONVIF 메타데이터로 사람 bbox 표시/클릭 선택
// - 선택된 bbox에서 v = top + h * RATIO (기본 0.26) 지점을 "Z=1200mm 평면"으로 가정
// - (u,v) 픽셀을 RBF(Thin-Plate Spline)로 PWM(pan,tilt)으로 보간
// - stdout 으로 "SET_PWM,PAN=...,TILT=..." 출력 (ubuntu_tcp_server 파이프로 전달)
// - (옵션) 월드(X,Y, cm) -> 픽셀(u,v) RBF를 이용해 Z=1200mm 평면 그리드 오버레이 표시

#include "RTSPClient.h"
#include "XMLParser.h"
#include "Config.h"

#include <opencv2/opencv.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cmath>

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_detect_all{false};

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
static int g_last_target_u = -1;
static int g_last_target_v = -1;
static bool g_last_pwm_valid = false;

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
            if (r <= 0) { total_read = 0; break; }
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
        if (!compute_rect_from_obj(obj, W, H, rect))
            continue;
        if (!rect.contains(cv::Point(x, y)))
            continue;

        {
            std::lock_guard<std::mutex> lock(g_sel_mutex);
            g_selected_id = obj.id;
            g_selected_rect = rect;
            g_selected_valid = true;
        }

        std::cerr << "SELECT "
                  << "id=" << obj.id
                  << " x=" << rect.x << " y=" << rect.y
                  << " w=" << rect.width << " h=" << rect.height
                  << std::endl;

        // 클릭 순간에는 아직 "선택된 rect 기준" PWM이 계산되기 전일 수 있음.
        // → 다음 프레임에서 계산된 값(화면 오버레이와 동일)을 출력하도록 pending 플래그만 세팅.
        {
            std::lock_guard<std::mutex> lk(g_click_mutex);
            g_click_pending = true;
            g_click_pending_id = obj.id;
        }
        break;
    }
}

// --- RBF (Thin-Plate Spline) interpolator for 2D -> scalar ---
// Model: f(x) = sum_i w_i * phi(||x - x_i||) + a0 + a1*x + a2*y
// phi(r) = r^2 * log(r + eps)
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
        // bottom-right 3x3 is zeros (already)

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
    // python calib_data: [X_cm, Y_cm, u, v, pan, tilt]
    const double data[][6] = {
        {-370,  65,   80,   405,   890,  1320}, // p0?
        {-370,  201,  401,  285,  1055,  1390}, // p1
        {-370,  406,  775,  191,  1220,  1420}, // p2
        {-370,  586,  970,  154,  1305,  1435}, // p3
        {-370,  880,  1154, 129,  1385,  1450}, // p4
        {-370,  990,  1201, 124,  1410,  1455}, // p5
        {-508,  990,  1085, 108,  1355,  1460}, // p6
        {0,     221,  1607, 550,  1605,  1235}, // p7
        {0,     571,  1608, 292,  1600,  1385}, // p8
        {0,     891,  1588, 221,  1585,  1430}, // p9
        {-234,  0,      5,  632,   870,  1170}, // p10
        {-234,  250,  785,  300,  1220,  1360}, // p11
        {-234,  560,  1163, 193,  1390,  1420}, // p12
        {-234,  870,  1310, 159,  1455,  1440}, // p13
        {-234,  1000, 1342, 149,  1470,  1450}, // p14
        {0,     100,  1535, 898,  1570,  1065}, // p15 (new)
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

    // grid step (cm)
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
            if (p.x < -2000 || p.x > W + 2000 || p.y < -2000 || p.y > H + 2000)
                continue;
            poly.push_back(p);
        }
        if (poly.size() >= 2)
            cv::polylines(frame, poly, false, cv::Scalar(80, 80, 220), 1, cv::LINE_AA);
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
            if (p.x < -2000 || p.x > W + 2000 || p.y < -2000 || p.y > H + 2000)
                continue;
            poly.push_back(p);
        }
        if (poly.size() >= 2)
            cv::polylines(frame, poly, false, cv::Scalar(80, 220, 80), 1, cv::LINE_AA);
    }

    // draw calib points
    for (const auto& p : pts)
    {
        cv::Point uv((int)std::lround(p.u), (int)std::lround(p.v));
        cv::circle(frame, uv, 4, cv::Scalar(0, 200, 255), -1, cv::LINE_AA);
        cv::putText(frame, p.name.c_str(), uv + cv::Point(6, -6),
                    cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0, 200, 255), 1);
    }
}

struct KalmanBbox2D
{
    double cx{0}, cy{0};
    double vx{0}, vy{0};
    double w{0}, h{0};
    double alpha_pos{0.6};
    double beta_vel{0.15};
    double alpha_size{0.3};
    bool initialized{false};

    void update(double meas_cx, double meas_cy, double meas_w, double meas_h, double dt)
    {
        if (!initialized || dt <= 0)
        {
            cx = meas_cx; cy = meas_cy;
            w = meas_w;   h = meas_h;
            vx = vy = 0;
            initialized = true;
            return;
        }
        double px = cx + vx * dt;
        double py = cy + vy * dt;
        double rx = meas_cx - px;
        double ry = meas_cy - py;
        cx = px + alpha_pos * rx;
        cy = py + alpha_pos * ry;
        vx += (beta_vel * rx) / dt;
        vy += (beta_vel * ry) / dt;
        w += alpha_size * (meas_w - w);
        h += alpha_size * (meas_h - h);
    }

    void predict(double dt_ahead, double& pred_cx, double& pred_cy) const
    {
        pred_cx = cx + vx * dt_ahead;
        pred_cy = cy + vy * dt_ahead;
    }

    void reset() { initialized = false; cx = cy = vx = vy = w = h = 0; }
};

int main(int argc, char** argv)
{
    double ratio = 0.3;
    double alpha = 0.5;
    int pan_min = 500, pan_max = 2500;
    int tilt_min = 500, tilt_max = 2500;
    int send_every_n = 1;
    bool draw_grid = true;
    double predict_ms = 300.0;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--detect-all") g_detect_all = true;
        else if (arg == "--ratio" && i + 1 < argc) ratio = std::atof(argv[++i]);
        else if (arg == "--alpha" && i + 1 < argc) alpha = std::atof(argv[++i]);
        else if (arg == "--send-every" && i + 1 < argc) send_every_n = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--no-grid") draw_grid = false;
        else if (arg == "--predict-ms" && i + 1 < argc) predict_ms = std::atof(argv[++i]);
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
    std::cerr << "[RBF] fitted N=" << px.size() << "\n";

    // RBF fit: world(X,Y, cm) -> pixel(u,v) for grid overlay
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
    RbfTps2D rbf_u, rbf_v;
    if (!rbf_u.fit(wxy, uu) || !rbf_v.fit(wxy, vv))
        std::cerr << "[GRID] world->pixel RBF fit failed (grid disabled)\n";

    // RTSP metadata
    RTSPClient client;
    XMLParser parser;
    if (!client.connectToCamera())
        return -1;
    client.sendHandshake();
    std::thread meta_thread(metadata_thread_fn, &client, &parser);

    // Video
    cv::VideoCapture cap(RTSP_URL);
    if (!cap.isOpened())
    {
        std::cerr << "[camera_RBF] RTSP open fail: " << RTSP_URL << "\n";
        g_running = false;
        meta_thread.join();
        return -1;
    }

    cv::namedWindow("camera_RBF", cv::WINDOW_NORMAL);
    cv::setMouseCallback("camera_RBF", on_mouse, &g_last_frame);

    int prev_pan = 1500, prev_tilt = 1500;
    int frame_id = 0;
    auto t_fps0 = std::chrono::steady_clock::now();
    auto t_last_frame = std::chrono::steady_clock::now();

    KalmanBbox2D kf;
    std::string prev_sel_id;

    while (g_running)
    {
        cv::Mat frame;
        if (!cap.read(frame) || frame.empty())
            break;

        {
            std::lock_guard<std::mutex> lock(g_frame_mutex);
            g_last_frame = frame.clone();
        }

        const int W = frame.cols;
        const int H = frame.rows;

        // copy objs
        std::vector<ParsedMetadataObject> objs;
        {
            std::lock_guard<std::mutex> lock(g_obj_mutex);
            objs = g_objects;
        }

        // draw boxes
        for (const auto& obj : objs)
        {
            cv::Rect r;
            if (!compute_rect_from_obj(obj, W, H, r)) continue;
            cv::rectangle(frame, r, cv::Scalar(0, 255, 255), 2);
            cv::putText(frame, obj.id.c_str(), cv::Point(r.x, std::max(0, r.y - 5)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 255), 1);
        }

        // selected rect
        std::string sel_id;
        cv::Rect sel_rect;
        bool sel_ok = false;
        {
            std::lock_guard<std::mutex> lock(g_sel_mutex);
            sel_ok = g_selected_valid;
            sel_id = g_selected_id;
            sel_rect = g_selected_rect;
        }

        if (sel_ok)
        {
            for (const auto& obj : objs)
            {
                if (obj.id == sel_id)
                {
                    cv::Rect updated;
                    if (compute_rect_from_obj(obj, W, H, updated))
                    {
                        sel_rect = updated;
                        std::lock_guard<std::mutex> lock(g_sel_mutex);
                        g_selected_rect = updated;
                    }
                    break;
                }
            }
        }

        auto t_now = std::chrono::steady_clock::now();
        double dt_sec = std::chrono::duration<double>(t_now - t_last_frame).count();
        t_last_frame = t_now;
        if (dt_sec <= 0 || dt_sec > 1.0) dt_sec = 1.0 / 30.0;

        if (sel_ok && sel_id != prev_sel_id)
        {
            kf.reset();
            prev_pan = 1500;
            prev_tilt = 1500;
            prev_sel_id = sel_id;
        }
        if (!sel_ok && !prev_sel_id.empty())
        {
            kf.reset();
            prev_sel_id.clear();
        }

        int target_u = W / 2;
        int target_v = H / 2;
        int pred_target_u = target_u;
        int pred_target_v = target_v;
        std::string src = "none";

        if (sel_ok)
        {
            sel_rect.x = std::max(0, std::min(sel_rect.x, W - 1));
            sel_rect.y = std::max(0, std::min(sel_rect.y, H - 1));
            sel_rect.width = std::max(1, std::min(sel_rect.width, W - sel_rect.x));
            sel_rect.height = std::max(1, std::min(sel_rect.height, H - sel_rect.y));

            cv::rectangle(frame, sel_rect, cv::Scalar(0, 255, 0), 2);
            cv::putText(frame, ("SEL " + sel_id).c_str(), cv::Point(sel_rect.x, std::max(0, sel_rect.y - 10)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);

            double bbox_cx = sel_rect.x + sel_rect.width * 0.5;
            double bbox_cy = sel_rect.y + sel_rect.height * ratio;
            bbox_cy = std::max(0.0, std::min((double)(H - 1), bbox_cy));

            target_u = (int)std::lround(bbox_cx);
            target_v = (int)std::lround(bbox_cy);
            src = "kalman";

            kf.update(bbox_cx, bbox_cy, (double)sel_rect.width, (double)sel_rect.height, dt_sec);

            double pcx, pcy;
            kf.predict(predict_ms / 1000.0, pcx, pcy);
            pcx = std::max(0.0, std::min((double)(W - 1), pcx));
            pcy = std::max(0.0, std::min((double)(H - 1), pcy));
            pred_target_u = (int)std::lround(pcx);
            pred_target_v = (int)std::lround(pcy);

            cv::line(frame, cv::Point(sel_rect.x, target_v), cv::Point(sel_rect.x + sel_rect.width, target_v),
                     cv::Scalar(0, 255, 0), 1);
        }

        // RBF -> PWM (predicted position)
        double pan_d = rbf_pan.eval((double)pred_target_u, (double)pred_target_v);
        double tilt_d = rbf_tilt.eval((double)pred_target_u, (double)pred_target_v);

        int pan = (int)std::lround(std::max((double)pan_min, std::min((double)pan_max, pan_d)));
        int tilt = (int)std::lround(std::max((double)tilt_min, std::min((double)tilt_max, tilt_d)));

        pan = (int)std::lround(alpha * pan + (1.0 - alpha) * prev_pan);
        tilt = (int)std::lround(alpha * tilt + (1.0 - alpha) * prev_tilt);
        prev_pan = pan;
        prev_tilt = tilt;

        {
            std::lock_guard<std::mutex> lk(g_pwm_mutex);
            g_last_pan = pan;
            g_last_tilt = tilt;
            g_last_target_u = pred_target_u;
            g_last_target_v = pred_target_v;
            g_last_pwm_valid = true;
        }

        if (sel_ok && frame_id % send_every_n == 0)
        {
            std::cout << "SET_PWM,PAN=" << pan << ",TILT=" << tilt << std::endl;
        }
        else if (frame_id % 30 == 0)
        {
            std::cout << std::endl;
        }
        if (!std::cout)
        {
            g_running = false;
            break;
        }

        {
            std::lock_guard<std::mutex> lk(g_click_mutex);
            if (g_click_pending && sel_ok && sel_id == g_click_pending_id)
            {
                std::cerr << "CLICK_PWM "
                          << "id=" << sel_id
                          << " meas=(" << target_u << "," << target_v << ")"
                          << " pred=(" << pred_target_u << "," << pred_target_v << ")"
                          << " vel=(" << std::fixed << std::setprecision(1) << kf.vx << "," << kf.vy << ")"
                          << " PAN=" << pan << " TILT=" << tilt
                          << std::endl;
                g_click_pending = false;
                g_click_pending_id.clear();
            }
        }

        // draw 1200mm plane grid overlay (world->pixel RBF)
        if (draw_grid)
            draw_world_grid(frame, rbf_u, rbf_v, pts);

        // draw measured target (orange, small)
        cv::circle(frame, cv::Point(target_u, target_v), 5, cv::Scalar(0, 165, 255), -1, cv::LINE_AA);
        // draw predicted target (magenta, big) + line from measured
        if (sel_ok && kf.initialized)
        {
            cv::line(frame, cv::Point(target_u, target_v), cv::Point(pred_target_u, pred_target_v),
                     cv::Scalar(255, 0, 255), 2, cv::LINE_AA);
            cv::circle(frame, cv::Point(pred_target_u, pred_target_v), 9, cv::Scalar(255, 0, 255), -1, cv::LINE_AA);
            cv::circle(frame, cv::Point(pred_target_u, pred_target_v), 9, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
        }
        else
        {
            cv::circle(frame, cv::Point(target_u, target_v), 8, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
        }

        char info[256];
        std::snprintf(info, sizeof(info), "src=%s predict=%.0fms pan=%d tilt=%d vx=%.0f vy=%.0f",
                      src.c_str(), predict_ms, pan, tilt, kf.vx, kf.vy);
        cv::putText(frame, info, cv::Point(10, 30),
                    cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 255), 2);

        // fps
        frame_id++;
        if (frame_id % 30 == 0)
        {
            auto t1 = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(t1 - t_fps0).count();
            double fps = (dt > 1e-6) ? (30.0 / dt) : 0.0;
            t_fps0 = t1;
            std::cerr << "[FPS] " << fps << "\n";
        }

        cv::imshow("camera_RBF", frame);
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

