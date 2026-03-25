#pragma once

#include <vector>
#include <opencv2/core.hpp>

struct CalibPoint {
    double X_cm{0.0}, Y_cm{0.0};
    double u{0.0}, v{0.0};
    double pan{0.0}, tilt{0.0};
};

std::vector<CalibPoint> loadCalibPointsBuiltin();

class RbfTps2D {
public:
    bool fit(const std::vector<cv::Point2d> &X, const std::vector<double> &Y);
    double eval(double x, double y) const;

private:
    static double phi(double r);
    std::vector<cv::Point2d> X_;
    std::vector<double> w_;
    double a0_{0.0}, a1_{0.0}, a2_{0.0};
};

struct KalmanBbox2D {
    double cx{0}, cy{0};
    double vx{0}, vy{0};
    double w{0}, h{0};
    double alpha_pos{0.6};
    double beta_vel{0.15};
    double alpha_size{0.3};
    double max_jump_px{120.0};
    double max_vel_px_s{2000.0};
    bool initialized{false};

    void update(double meas_cx, double meas_cy, double meas_w, double meas_h, double dt);
    void predict(double dt_ahead, double &pred_cx, double &pred_cy) const;
    void reset();
};

struct ParsedMetadataObject;

bool computeRectFromObj(const ParsedMetadataObject &obj, int W, int H, cv::Rect &out);
