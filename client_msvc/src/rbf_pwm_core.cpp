#include "rbf_pwm_core.h"
#include "XMLParser.h"
#include "Config.h"

#include <opencv2/core.hpp>
#include <algorithm>
#include <cmath>

std::vector<CalibPoint> loadCalibPointsBuiltin()
{
    const double data[][6] = {
        {-370, 65, 80, 405, 890, 1320},    {-370, 201, 401, 285, 1055, 1390},
        {-370, 406, 775, 191, 1220, 1420}, {-370, 586, 970, 154, 1305, 1435},
        {-370, 880, 1154, 129, 1385, 1450}, {-370, 990, 1201, 124, 1410, 1455},
        {-508, 990, 1085, 108, 1355, 1460}, {0, 221, 1607, 550, 1605, 1235},
        {0, 571, 1608, 292, 1600, 1385},    {0, 891, 1588, 221, 1585, 1430},
        {-234, 0, 5, 632, 870, 1170},      {-234, 250, 785, 300, 1220, 1360},
        {-234, 560, 1163, 193, 1390, 1420}, {-234, 870, 1310, 159, 1455, 1440},
        {-234, 1000, 1342, 149, 1470, 1450}, {0, 100, 1535, 898, 1570, 1065},
    };
    std::vector<CalibPoint> pts;
    const int N = static_cast<int>(sizeof(data) / sizeof(data[0]));
    pts.reserve(N);
    for (int i = 0; i < N; ++i) {
        CalibPoint p;
        p.X_cm = data[i][0];
        p.Y_cm = data[i][1];
        p.u = data[i][2];
        p.v = data[i][3];
        p.pan = data[i][4];
        p.tilt = data[i][5];
        pts.push_back(p);
    }
    return pts;
}

bool RbfTps2D::fit(const std::vector<cv::Point2d> &X, const std::vector<double> &Y)
{
    const int N = static_cast<int>(X.size());
    if (N < 4 || static_cast<int>(Y.size()) != N)
        return false;

    cv::Mat A = cv::Mat::zeros(N + 3, N + 3, CV_64F);
    cv::Mat b = cv::Mat::zeros(N + 3, 1, CV_64F);

    for (int i = 0; i < N; ++i) {
        b.at<double>(i, 0) = Y[i];
        for (int j = 0; j < N; ++j) {
            double dx = X[i].x - X[j].x;
            double dy = X[i].y - X[j].y;
            double r = std::sqrt(dx * dx + dy * dy);
            A.at<double>(i, j) = phi(r);
        }
        A.at<double>(i, N + 0) = 1.0;
        A.at<double>(i, N + 1) = X[i].x;
        A.at<double>(i, N + 2) = X[i].y;
    }

    for (int j = 0; j < N; ++j) {
        A.at<double>(N + 0, j) = 1.0;
        A.at<double>(N + 1, j) = X[j].x;
        A.at<double>(N + 2, j) = X[j].y;
    }

    cv::Mat x;
    if (!cv::solve(A, b, x, cv::DECOMP_SVD))
        return false;

    w_.assign(N, 0.0);
    for (int i = 0; i < N; ++i)
        w_[i] = x.at<double>(i, 0);
    a0_ = x.at<double>(N + 0, 0);
    a1_ = x.at<double>(N + 1, 0);
    a2_ = x.at<double>(N + 2, 0);
    X_ = X;
    return true;
}

double RbfTps2D::eval(double x, double y) const
{
    const int N = static_cast<int>(X_.size());
    double s = 0.0;
    for (int i = 0; i < N; ++i) {
        double dx = x - X_[i].x;
        double dy = y - X_[i].y;
        double r = std::sqrt(dx * dx + dy * dy);
        s += w_[i] * phi(r);
    }
    s += a0_ + a1_ * x + a2_ * y;
    return s;
}

double RbfTps2D::phi(double r)
{
    const double eps = 1e-6;
    double rr = r * r;
    return rr * std::log(r + eps);
}

void KalmanBbox2D::update(double meas_cx, double meas_cy, double meas_w, double meas_h, double dt)
{
    if (!initialized || dt <= 0) {
        cx = meas_cx;
        cy = meas_cy;
        w = meas_w;
        h = meas_h;
        vx = vy = 0;
        initialized = true;
        return;
    }

    dt = std::max(dt, 1e-4);

    double px = cx + vx * dt;
    double py = cy + vy * dt;
    double rx = meas_cx - px;
    double ry = meas_cy - py;

    const double dist2 = rx * rx + ry * ry;
    if (dist2 > max_jump_px * max_jump_px) {
        cx = meas_cx;
        cy = meas_cy;
        w = meas_w;
        h = meas_h;
        vx = 0;
        vy = 0;
        return;
    }

    cx = px + alpha_pos * rx;
    cy = py + alpha_pos * ry;
    vx += (beta_vel * rx) / dt;
    vy += (beta_vel * ry) / dt;

    vx = std::max(-max_vel_px_s, std::min(max_vel_px_s, vx));
    vy = std::max(-max_vel_px_s, std::min(max_vel_px_s, vy));

    w += alpha_size * (meas_w - w);
    h += alpha_size * (meas_h - h);
}

void KalmanBbox2D::predict(double dt_ahead, double &pred_cx, double &pred_cy) const
{
    pred_cx = cx + vx * dt_ahead;
    pred_cy = cy + vy * dt_ahead;
}

void KalmanBbox2D::reset()
{
    initialized = false;
    cx = cy = vx = vy = w = h = 0;
}

bool computeRectFromObj(const ParsedMetadataObject &obj, int W, int H, cv::Rect &out)
{
    int left, right, top, bottom;
    if (std::max({obj.left, obj.right, obj.top, obj.bottom}) <= 1.5f) {
        left = static_cast<int>(obj.left * W);
        right = static_cast<int>(obj.right * W);
        top = static_cast<int>(obj.top * H);
        bottom = static_cast<int>(obj.bottom * H);
    } else {
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

    out.x = std::max(0, std::min(out.x, W - 1));
    out.y = std::max(0, std::min(out.y, H - 1));
    out.width = std::max(1, std::min(out.width, W - out.x));
    out.height = std::max(1, std::min(out.height, H - out.y));
    return (out.area() > 20);
}
