#include "../inc/img_processing.h"
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/dnn.hpp>

// =====================================================================
// [RAW ISP 경로] — CV_16UC1 입력 전용
// main에서 g_raw_mode == true 일 때만 호출됩니다.
// =====================================================================

// 1. BLC + AE/AWB 엔진
static void applyInitialISP(uint16_t* raw_buf, int width, int height, const ISPConfig& cfg) {
    long long sum_r = 0, sum_g = 0, sum_b = 0;
    int cnt_r = 0, cnt_g = 0, cnt_b = 0;

    for (int y = 0; y < height - 1; y += 4) {
        for (int x = 0; x < width - 1; x += 4) {
            auto clamp_blc = [&](uint32_t p) {
                return (p > cfg.black_level) ? (p - cfg.black_level) : 0u;
            };
            // SBGGR 패턴 2x2 블록
            sum_b += clamp_blc(raw_buf[y * width + x]);
            sum_g += clamp_blc(raw_buf[y * width + x + 1]);
            sum_g += clamp_blc(raw_buf[(y + 1) * width + x]);
            sum_r += clamp_blc(raw_buf[(y + 1) * width + x + 1]);
            cnt_b++; cnt_g += 2; cnt_r++;
        }
    }

    float avg_r = (cnt_r > 0) ? (float)sum_r / cnt_r : 1.0f;
    float avg_g = (cnt_g > 0) ? (float)sum_g / cnt_g : 1.0f;
    float avg_b = (cnt_b > 0) ? (float)sum_b / cnt_b : 1.0f;

    float dynamic_r_gain  = avg_g / (avg_r + 1.0f);
    float dynamic_b_gain  = avg_g / (avg_b + 1.0f);
    float current_brightness = (avg_r + avg_g + avg_b) / 3.0f;
    float ae_gain = std::max(0.5f, std::min(3.0f, 150.0f / (current_brightness + 1.0f)));

    float final_r_gain = dynamic_r_gain * ae_gain;
    float final_g_gain = 1.0f * ae_gain;
    float final_b_gain = dynamic_b_gain * ae_gain;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = y * width + x;
            uint32_t orig_pixel = raw_buf[idx];
            uint32_t pixel = (orig_pixel > cfg.black_level) ? (orig_pixel - cfg.black_level) : 0u;

            float gain = 1.0f;
            if (y % 2 == 0) gain = (x % 2 == 0) ? final_b_gain : final_g_gain;
            else             gain = (x % 2 == 0) ? final_g_gain : final_r_gain;

            // 하이라이트 롤오프 (950~1023)
            if (orig_pixel > 950) {
                float blend = std::min(1.0f, (orig_pixel - 950) / 73.0f);
                gain = gain * (1.0f - blend) + final_g_gain * blend;
            }

            raw_buf[idx] = (uint16_t)std::min((uint32_t)(pixel * gain), (uint32_t)1023);
        }
    }
}

// 2. Demosaicing (Bilinear Interpolation)
static std::vector<uint8_t> applyPureDemosaic(const uint16_t* raw_buf, int width, int height) {
    std::vector<uint8_t> bgr_buf(width * height * 3, 0);

    auto get_val = [&](int y, int x) -> uint8_t {
        y = std::max(0, std::min(y, height - 1));
        x = std::max(0, std::min(x, width  - 1));
        return (uint8_t)(raw_buf[y * width + x] >> 2);
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = (y * width + x) * 3;
            uint8_t r = 0, g = 0, b = 0;

            if (y % 2 == 0) {
                if (x % 2 == 0) { // Blue
                    b = get_val(y, x);
                    g = (get_val(y,x-1)+get_val(y,x+1)+get_val(y-1,x)+get_val(y+1,x)) / 4;
                    r = (get_val(y-1,x-1)+get_val(y-1,x+1)+get_val(y+1,x-1)+get_val(y+1,x+1)) / 4;
                } else {          // G1
                    b = (get_val(y,x-1)+get_val(y,x+1)) / 2;
                    g = get_val(y, x);
                    r = (get_val(y-1,x)+get_val(y+1,x)) / 2;
                }
            } else {
                if (x % 2 == 0) { // G2
                    b = (get_val(y-1,x)+get_val(y+1,x)) / 2;
                    g = get_val(y, x);
                    r = (get_val(y,x-1)+get_val(y,x+1)) / 2;
                } else {          // Red
                    b = (get_val(y-1,x-1)+get_val(y-1,x+1)+get_val(y+1,x-1)+get_val(y+1,x+1)) / 4;
                    g = (get_val(y,x-1)+get_val(y,x+1)+get_val(y-1,x)+get_val(y+1,x)) / 4;
                    r = get_val(y, x);
                }
            }
            bgr_buf[idx]   = b;
            bgr_buf[idx+1] = g;
            bgr_buf[idx+2] = r;
        }
    }
    return bgr_buf;
}

// 3. CCM (Color Correction Matrix)
struct CCMConfig {
    float ccm[3][3] = {
        {  1.61f, -0.40f, -0.21f },
        { -0.27f,  1.48f, -0.21f },
        { -0.08f, -0.52f,  1.60f }
    };
};

static void applyCCM(std::vector<uint8_t>& bgr_buf, int width, int height, const CCMConfig& cfg) {
    for (int i = 0; i < width * height * 3; i += 3) {
        float b = bgr_buf[i], g = bgr_buf[i+1], r = bgr_buf[i+2];

        // 하이라이트 영역 CCM 완화 (색 왜곡 방지)
        float max_val = std::max({r, g, b});
        if (max_val > 200.0f) {
            float blend = std::min(1.0f, (max_val - 200.0f) / 55.0f);
            float avg = (r + g + b) / 3.0f;
            r = r*(1-blend) + avg*blend;
            g = g*(1-blend) + avg*blend;
            b = b*(1-blend) + avg*blend;
        }

        bgr_buf[i]   = (uint8_t)std::max(0.0f, std::min(255.0f, r*cfg.ccm[2][0]+g*cfg.ccm[2][1]+b*cfg.ccm[2][2]));
        bgr_buf[i+1] = (uint8_t)std::max(0.0f, std::min(255.0f, r*cfg.ccm[1][0]+g*cfg.ccm[1][1]+b*cfg.ccm[1][2]));
        bgr_buf[i+2] = (uint8_t)std::max(0.0f, std::min(255.0f, r*cfg.ccm[0][0]+g*cfg.ccm[0][1]+b*cfg.ccm[0][2]));
    }
}

// 4. RGB Gamma (LUT 방식)
static void applyRGBGamma(std::vector<uint8_t>& bgr_buf, int width, int height, float gamma = 2.2f) {
    uint8_t lut[256];
    for (int i = 0; i < 256; i++) {
        float v = std::pow(i / 255.0f, 1.0f / gamma);
        lut[i] = (uint8_t)std::min(255.0f, v * 255.0f);
    }
    for (int i = 0; i < width * height * 3; i++)
        bgr_buf[i] = lut[bgr_buf[i]];
}

// =====================================================================
// runPureISP — 공개 인터페이스
// 입력: CV_16UC1 RAW 프레임 (const 참조, 원본 보존)
// 출력: CV_8UC3 BGR 이미지
// =====================================================================
cv::Mat runPureISP(const cv::Mat& raw16_frame) {
    if (raw16_frame.empty() || raw16_frame.type() != CV_16UC1) {
        std::cerr << "🚨 runPureISP: CV_16UC1이 아닌 입력입니다!" << std::endl;
        return raw16_frame;
    }

    // 원본을 훼손하지 않기 위해 복사본에서 작업
    cv::Mat work = raw16_frame.clone();
    int width    = work.cols;
    int height   = work.rows;
    uint16_t* raw_data = (uint16_t*)work.data;

    ISPConfig cfg;
    applyInitialISP(raw_data, width, height, cfg);           // BLC + AWB + AE

    std::vector<uint8_t> bgr = applyPureDemosaic(raw_data, width, height); // Demosaic

    CCMConfig ccm_cfg;
    applyCCM(bgr, width, height, ccm_cfg);                   // CCM

    applyRGBGamma(bgr, width, height, 2.2f);                 // Gamma

    cv::Mat out(height, width, CV_8UC3);
    std::copy(bgr.begin(), bgr.end(), out.data);
    return out;
}

// =====================================================================
// [후처리 경로] — CV_8UC3 입력 (RAW ISP 결과 or BGR 직접 입력 모두 수용)
// =====================================================================

// 5. Shadow Boost (Gamma + Tone Mapping, YUV Y채널만 처리)
void applyShadowBoost(const cv::Mat& src, cv::Mat& dst, double gamma, double alpha) {
    if (src.empty()) return;

    cv::Mat yuv;
    cv::cvtColor(src, yuv, cv::COLOR_BGR2YUV);

    unsigned char lut[256];
    for (int i = 0; i < 256; i++) {
        double I_in     = i / 255.0;
        double I_boost  = std::pow(I_in, 1.0 / gamma);
        I_boost         = alpha * (I_boost - 0.5) + 0.5;
        double sw       = 1.0 - std::pow(I_in, 2.0);
        double final_I  = I_boost * sw + I_in * (1.0 - sw);
        lut[i] = cv::saturate_cast<unsigned char>(final_I * 255.0);
    }

    int rows = yuv.rows, cols = yuv.cols;
    if (yuv.isContinuous()) { cols *= rows; rows = 1; }
    for (int r = 0; r < rows; r++) {
        cv::Vec3b* ptr = yuv.ptr<cv::Vec3b>(r);
        for (int c = 0; c < cols; c++)
            ptr[c][0] = lut[ptr[c][0]];
    }
    cv::cvtColor(yuv, dst, cv::COLOR_YUV2BGR);
}

// 6. CLAHE (직접 구현, Bilinear 타일 보간 포함)
void applyCLAHE(const cv::Mat& src, cv::Mat& dst, double clip_limit, cv::Size grid) {
    cv::Mat lab;
    cv::cvtColor(src, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> planes;
    cv::split(lab, planes);
    cv::Mat& L = planes[0];

    int rows = L.rows, cols = L.cols;
    int gx = grid.width, gy = grid.height;
    int tw = (int)std::ceil((float)cols / gx);
    int th = (int)std::ceil((float)rows / gy);
    int clip_thr = std::max(1, (int)(clip_limit * tw * th / 256.0));

    // 타일별 CDF 계산
    std::vector<std::vector<std::vector<int>>>
        cdfs(gy, std::vector<std::vector<int>>(gx, std::vector<int>(256, 0)));

    for (int ty = 0; ty < gy; ty++) {
        for (int tx = 0; tx < gx; tx++) {
            int hist[256] = {0};
            int y0 = ty*th, y1 = std::min(rows, y0+th);
            int x0 = tx*tw, x1 = std::min(cols, x0+tw);
            int npix = (y1-y0)*(x1-x0);

            for (int y = y0; y < y1; y++) {
                const uchar* p = L.ptr<uchar>(y);
                for (int x = x0; x < x1; x++) hist[p[x]]++;
            }

            // 클리핑 + 재분배
            int clipped = 0;
            for (int i = 0; i < 256; i++) {
                if (hist[i] > clip_thr) { clipped += hist[i]-clip_thr; hist[i] = clip_thr; }
            }
            int step = clipped/256, residual = clipped%256;
            for (int i = 0; i < 256; i++) {
                hist[i] += step;
                if (i < residual) hist[i]++;
            }

            int sum = 0;
            for (int i = 0; i < 256; i++) {
                sum += hist[i];
                cdfs[ty][tx][i] = (sum * 255) / npix;
            }
        }
    }

    // Bilinear 보간 적용
    cv::Mat out_L = L.clone();
    for (int y = 0; y < rows; y++) {
        uchar* op = out_L.ptr<uchar>(y);
        const uchar* ip = L.ptr<uchar>(y);
        float ty_f = (float)y/th - 0.5f;
        int ty1 = std::max(0, (int)std::floor(ty_f));
        int ty2 = std::min(gy-1, ty1+1);
        float yr = (ty_f < 0) ? 0 : ty_f - ty1;

        for (int x = 0; x < cols; x++) {
            float tx_f = (float)x/tw - 0.5f;
            int tx1 = std::max(0, (int)std::floor(tx_f));
            int tx2 = std::min(gx-1, tx1+1);
            float xr = (tx_f < 0) ? 0 : tx_f - tx1;

            uchar v = ip[x];
            float top = cdfs[ty1][tx1][v]*(1-xr) + cdfs[ty1][tx2][v]*xr;
            float bot = cdfs[ty2][tx1][v]*(1-xr) + cdfs[ty2][tx2][v]*xr;
            op[x] = cv::saturate_cast<uchar>(top*(1-yr) + bot*yr);
        }
    }

    planes[0] = out_L;
    cv::merge(planes, lab);
    cv::cvtColor(lab, dst, cv::COLOR_Lab2BGR);
}

// 7. 엔트로피 계산
static double calculateEntropy(const cv::Mat& frame) {
    if (frame.empty()) return 0.0;
    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

    int histSize = 256;
    float range[] = {0, 256};
    const float* hr = {range};
    cv::Mat hist;
    cv::calcHist(&gray, 1, 0, cv::Mat(), hist, 1, &histSize, &hr, true, false);
    hist /= (gray.rows * gray.cols);

    double entropy = 0.0;
    for (int i = 0; i < histSize; i++) {
        float p = hist.at<float>(i);
        if (p > 0.0f) entropy -= p * std::log2(p);
    }
    return entropy;
}

// 8. 픽셀 분포 분석 — 보정 전/후 밝기 분포 변화를 정량적으로 로깅
struct PixelDistStats {
    double mean;           // 평균 밝기
    double stddev;         // 표준 편차
    double pct_below_50;   // 밝기 0~49 구간 비율 (%)
    double pct_50_100;     // 밝기 50~99 구간 비율 (%)
    double pct_100_150;    // 밝기 100~149 구간 비율 (%)
    double pct_150_200;    // 밝기 150~199 구간 비율 (%)
    double pct_above_200;  // 밝기 200~255 구간 비율 (%)
};

static PixelDistStats analyzePixelDistribution(const cv::Mat& bgr_frame) {
    PixelDistStats st{};
    if (bgr_frame.empty()) return st;

    cv::Mat gray;
    cv::cvtColor(bgr_frame, gray, cv::COLOR_BGR2GRAY);

    cv::Scalar mu, sigma;
    cv::meanStdDev(gray, mu, sigma);
    st.mean   = mu[0];
    st.stddev = sigma[0];

    int total = gray.rows * gray.cols;
    int bins[5] = {0};  // [0-49] [50-99] [100-149] [150-199] [200-255]

    for (int r = 0; r < gray.rows; r++) {
        const uchar* p = gray.ptr<uchar>(r);
        for (int c = 0; c < gray.cols; c++) {
            int v = p[c];
            if      (v <  50) bins[0]++;
            else if (v < 100) bins[1]++;
            else if (v < 150) bins[2]++;
            else if (v < 200) bins[3]++;
            else              bins[4]++;
        }
    }

    st.pct_below_50  = 100.0 * bins[0] / total;
    st.pct_50_100    = 100.0 * bins[1] / total;
    st.pct_100_150   = 100.0 * bins[2] / total;
    st.pct_150_200   = 100.0 * bins[3] / total;
    st.pct_above_200 = 100.0 * bins[4] / total;
    return st;
}

static void logDistComparison(const std::string& label_before,
                              const PixelDistStats& before,
                              const std::string& label_after,
                              const PixelDistStats& after) {
    std::cout << "──────────── 픽셀 분포 분석 ────────────" << std::endl;
    std::cout << "[" << label_before << "] "
              << "mean=" << std::fixed << std::setprecision(1) << before.mean
              << "  stddev=" << before.stddev << std::endl;
    std::cout << "   0~ 49: " << std::setprecision(1) << before.pct_below_50  << "% | "
              << " 50~ 99: " << before.pct_50_100    << "% | "
              << "100~149: " << before.pct_100_150   << "% | "
              << "150~199: " << before.pct_150_200   << "% | "
              << "200~255: " << before.pct_above_200 << "%" << std::endl;
    std::cout << "[" << label_after << "] "
              << "mean=" << after.mean
              << "  stddev=" << after.stddev << std::endl;
    std::cout << "   0~ 49: " << after.pct_below_50  << "% | "
              << " 50~ 99: " << after.pct_50_100    << "% | "
              << "100~149: " << after.pct_100_150   << "% | "
              << "150~199: " << after.pct_150_200   << "% | "
              << "200~255: " << after.pct_above_200 << "%" << std::endl;
    std::cout << "─────────────────────────────────────────" << std::endl;
}

// 9. 히스토그램 이미지 저장 — 보정 전/후 밝기 분포를 시각적으로 비교
static cv::Mat computeGrayHist(const cv::Mat& bgr_frame) {
    cv::Mat gray;
    cv::cvtColor(bgr_frame, gray, cv::COLOR_BGR2GRAY);
    int histSize = 256;
    float range[] = {0, 256};
    const float* hr = {range};
    cv::Mat hist;
    cv::calcHist(&gray, 1, 0, cv::Mat(), hist, 1, &histSize, &hr, true, false);
    cv::normalize(hist, hist, 0, 1, cv::NORM_MINMAX);
    return hist;
}

static void saveHistogramComparison(const cv::Mat& before,
                                    const cv::Mat& after,
                                    const PixelDistStats& st_before,
                                    const PixelDistStats& st_after,
                                    int best_idx,
                                    const std::string& save_path) {
    // 캔버스 설정
    const int W = 1200, H = 500;
    const int margin_l = 60, margin_r = 30, margin_t = 70, margin_b = 70;
    const int plot_w = W - margin_l - margin_r;
    const int plot_h = H - margin_t - margin_b;

    cv::Mat canvas(H, W, CV_8UC3, cv::Scalar(255, 255, 255));

    // 히스토그램 데이터
    cv::Mat hist_before = computeGrayHist(before);
    cv::Mat hist_after  = computeGrayHist(after);

    // 색상 정의
    const cv::Scalar col_before(200, 180, 180);   // 연한 파란 계열 (BGR)
    const cv::Scalar col_after (180, 210, 130);    // 연한 초록 계열
    const cv::Scalar line_before(180, 80, 80);     // 진한 파란 (라인)
    const cv::Scalar line_after (60, 160, 60);     // 진한 초록 (라인)

    // 막대 그리기 (반투명 효과를 위해 두 히스토그램을 겹침)
    float bar_w = (float)plot_w / 256.0f;
    for (int i = 0; i < 256; i++) {
        float vb = hist_before.at<float>(i);
        float va = hist_after.at<float>(i);

        int x1 = margin_l + (int)(i * bar_w);
        int x2 = margin_l + (int)((i + 1) * bar_w);

        int yb = margin_t + plot_h - (int)(vb * plot_h);
        int ya = margin_t + plot_h - (int)(va * plot_h);
        int y_base = margin_t + plot_h;

        // 보정 전 (배경)
        cv::rectangle(canvas, {x1, yb}, {x2, y_base}, col_before, cv::FILLED);
        // 보정 후 (전경, 반투명 느낌)
        cv::rectangle(canvas, {x1, ya}, {x2, y_base}, col_after, cv::FILLED);
    }

    // 꺾은선 오버레이 (가독성 향상)
    for (int i = 1; i < 256; i++) {
        int x_prev = margin_l + (int)((i - 0.5f) * bar_w);
        int x_curr = margin_l + (int)((i + 0.5f) * bar_w);

        float vb0 = hist_before.at<float>(i - 1), vb1 = hist_before.at<float>(i);
        float va0 = hist_after.at<float>(i - 1),  va1 = hist_after.at<float>(i);

        cv::line(canvas,
                 {x_prev, margin_t + plot_h - (int)(vb0 * plot_h)},
                 {x_curr, margin_t + plot_h - (int)(vb1 * plot_h)},
                 line_before, 2, cv::LINE_AA);
        cv::line(canvas,
                 {x_prev, margin_t + plot_h - (int)(va0 * plot_h)},
                 {x_curr, margin_t + plot_h - (int)(va1 * plot_h)},
                 line_after, 2, cv::LINE_AA);
    }

    // 축, 그리드
    cv::line(canvas, {margin_l, margin_t}, {margin_l, margin_t + plot_h}, {0,0,0}, 1);
    cv::line(canvas, {margin_l, margin_t + plot_h}, {margin_l + plot_w, margin_t + plot_h}, {0,0,0}, 1);

    for (int v = 0; v <= 255; v += 50) {
        int x = margin_l + (int)(v * bar_w);
        cv::line(canvas, {x, margin_t + plot_h}, {x, margin_t + plot_h + 5}, {0,0,0}, 1);
        cv::putText(canvas, std::to_string(v),
                    {x - 10, margin_t + plot_h + 22},
                    cv::FONT_HERSHEY_SIMPLEX, 0.45, {0,0,0}, 1, cv::LINE_AA);
    }
    cv::putText(canvas, "Pixel Intensity",
                {margin_l + plot_w / 2 - 55, H - 10},
                cv::FONT_HERSHEY_SIMPLEX, 0.55, {0,0,0}, 1, cv::LINE_AA);

    // 제목
    cv::putText(canvas, "Pixel Distribution : Before vs After Correction",
                {margin_l, 30},
                cv::FONT_HERSHEY_SIMPLEX, 0.75, {0,0,0}, 2, cv::LINE_AA);

    // 범례
    int lx = margin_l + plot_w - 340, ly = margin_t + 15;
    cv::rectangle(canvas, {lx, ly}, {lx + 14, ly + 14}, line_before, cv::FILLED);
    cv::putText(canvas,
                cv::format("Before  (mean=%.1f, std=%.1f)", st_before.mean, st_before.stddev),
                {lx + 20, ly + 12}, cv::FONT_HERSHEY_SIMPLEX, 0.45, {0,0,0}, 1, cv::LINE_AA);

    ly += 22;
    cv::rectangle(canvas, {lx, ly}, {lx + 14, ly + 14}, line_after, cv::FILLED);
    cv::putText(canvas,
                cv::format("After #%d (mean=%.1f, std=%.1f)", best_idx, st_after.mean, st_after.stddev),
                {lx + 20, ly + 12}, cv::FONT_HERSHEY_SIMPLEX, 0.45, {0,0,0}, 1, cv::LINE_AA);

    cv::imwrite(save_path, canvas);
    std::cout << "[camera] histogram saved: " << save_path << std::endl;
}

// =====================================================================
// processISPAndGetBest — 공개 인터페이스
// 입력: CV_8UC3 BGR (runPureISP 결과 or libcamera BGR 직접 입력)
// 출력: best_frame (엔트로피 최고 후보), tuning_viesw_out (8분할 비교 뷰)
// =====================================================================
cv::Mat processISPAndGetBest(const cv::Mat& raw_frame_in, cv::Mat& tuning_view_out) {
    if (raw_frame_in.empty()) return raw_frame_in;

    // 후보 0: 원본 (ISP 완료 이미지)
    std::vector<cv::Mat> candidates(8);
    candidates[0] = raw_frame_in.clone();

    // 후보 1~7: ShadowBoost(gamma, alpha) + CLAHE(clip, grid)
    const float params[7][2] = {
        {1.2f, 1.0f}, {1.5f, 1.2f}, {1.8f, 1.4f},
        {2.2f, 1.6f}, {2.5f, 1.8f}, {2.8f, 2.0f}, {3.0f, 2.2f}
    };
    const double clips[7] = {1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5};

    cv::Mat t_boost;
    for (int i = 0; i < 7; i++) {
        applyShadowBoost(raw_frame_in, t_boost, params[i][0], params[i][1]);
        applyCLAHE(t_boost, candidates[i+1], clips[i], cv::Size(8,8));
    }

    // 엔트로피 평가
    double max_entropy = -1.0;
    int best_idx = 0;
    std::vector<double> entropies(8);
    for (int i = 0; i < 8; i++) {
        entropies[i] = calculateEntropy(candidates[i]);
        if (entropies[i] > max_entropy) { max_entropy = entropies[i]; best_idx = i; }
    }

    // 픽셀 분포 분석: 보정 전(원본) vs 보정 후(최적 후보) 비교 로깅
    PixelDistStats dist_before = analyzePixelDistribution(candidates[0]);
    PixelDistStats dist_after  = analyzePixelDistribution(candidates[best_idx]);
    logDistComparison("보정 전 (ISP Out)", dist_before,
                      "보정 후 (Best #" + std::to_string(best_idx) + ")", dist_after);
    saveHistogramComparison(candidates[0], candidates[best_idx],
                            dist_before, dist_after, best_idx,
                            "pixel_distribution_comparison.jpg");

    // Tuning View (4x2 그리드)
    double scale = 1920.0 / raw_frame_in.cols;
    cv::Mat raw_resized;
    cv::resize(raw_frame_in, raw_resized, cv::Size(), scale, scale, cv::INTER_AREA);
    int qr = raw_resized.rows/2, qc = raw_resized.cols/2;
    tuning_view_out = cv::Mat(qr*2, qc*4, CV_8UC3, cv::Scalar(0,0,0));

    const std::string titles[8] = {
        "1. ISP Out", "2. Lv.1", "3. Lv.2", "4. Lv.3",
        "5. Lv.4",   "6. Lv.5", "7. Lv.6", "8. Lv.7"
    };
    const std::string subs[8] = {
        "original", "(G=1.2,A=1.0)", "(G=1.5,A=1.2)", "(G=1.8,A=1.4)",
        "(G=2.2,A=1.6)", "(G=2.5,A=1.8)", "(G=2.8,A=2.0)", "(G=3.0,A=2.2)"
    };

    for (int i = 0; i < 8; i++) {
        cv::Mat q;
        cv::resize(candidates[i], q, cv::Size(qc, qr));
        cv::Scalar col = (i == best_idx) ? cv::Scalar(0,255,0) : cv::Scalar(0,0,255);

        cv::putText(q, titles[i], {15,35}, cv::FONT_HERSHEY_SIMPLEX, 0.9, {0,0,0}, 2, cv::LINE_AA);
        cv::putText(q, subs[i],   {15,70}, cv::FONT_HERSHEY_SIMPLEX, 0.7, col,    2, cv::LINE_AA);
        cv::putText(q, cv::format("Entropy: %.2f", entropies[i]),
                    {15,105}, cv::FONT_HERSHEY_SIMPLEX, 0.8, col, 2, cv::LINE_AA);

        q.copyTo(tuning_view_out(cv::Rect((i%4)*qc, (i/4)*qr, qc, qr)));
    }

    return candidates[best_idx];
}