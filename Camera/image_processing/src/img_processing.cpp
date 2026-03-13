#include "../inc/img_processing.h"
#include <cmath>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/dnn.hpp>

// ========================================================
// 🌟 [100% 순수 C++ 영역] 하드웨어 레벨 ISP 엔진
// ========================================================

// 1. 순수 C++ BLC & AWB 엔진 (기존과 동일)
void applyInitialISP(uint16_t* raw_buf, int width, int height, const ISPConfig& cfg) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = y * width + x;
            uint32_t pixel = raw_buf[idx];

            pixel = (pixel > cfg.black_level) ? (pixel - cfg.black_level) : 0;

            if (y % 2 == 0) { // B G 라인
                if (x % 2 == 0) pixel = (uint32_t)(pixel * cfg.b_gain); 
                else            pixel = (uint32_t)(pixel * cfg.g_gain); 
            } else {          // G R 라인
                if (x % 2 == 0) pixel = (uint32_t)(pixel * cfg.g_gain); 
                else            pixel = (uint32_t)(pixel * cfg.r_gain); 
            }

            raw_buf[idx] = (uint16_t)std::min(pixel, (uint32_t)1023);
        }
    }
}

// 🌟 2. NEW: 순수 C++ 수동 Demosaicing (Bilinear Interpolation)
// OpenCV를 쓰지 않고 주변 픽셀을 참조해 R, G, B 채널을 직접 조립합니다.
std::vector<uint8_t> applyPureDemosaic(const uint16_t* raw_buf, int width, int height) {
    std::vector<uint8_t> bgr_buf(width * height * 3, 0);

    // 경계값을 안전하게 가져오면서 10-bit -> 8-bit 스케일링을 수행하는 람다 함수
    auto get_val = [&](int y, int x) -> uint8_t {
        y = std::max(0, std::min(y, height - 1));
        x = std::max(0, std::min(x, width - 1));
        return (uint8_t)(raw_buf[y * width + x] >> 2); 
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = (y * width + x) * 3;
            uint8_t r = 0, g = 0, b = 0;

            // SBGGR 패턴에 따른 선형 보간 수학식 구현
            if (y % 2 == 0) { 
                if (x % 2 == 0) { // Blue 픽셀 (B)
                    b = get_val(y, x);
                    g = (get_val(y, x-1) + get_val(y, x+1) + get_val(y-1, x) + get_val(y+1, x)) / 4;
                    r = (get_val(y-1, x-1) + get_val(y-1, x+1) + get_val(y+1, x-1) + get_val(y+1, x+1)) / 4;
                } else { // Green 픽셀 (G1)
                    b = (get_val(y, x-1) + get_val(y, x+1)) / 2;
                    g = get_val(y, x);
                    r = (get_val(y-1, x) + get_val(y+1, x)) / 2;
                }
            } else { 
                if (x % 2 == 0) { // Green 픽셀 (G2)
                    b = (get_val(y-1, x) + get_val(y+1, x)) / 2;
                    g = get_val(y, x);
                    r = (get_val(y, x-1) + get_val(y, x+1)) / 2;
                } else { // Red 픽셀 (R)
                    b = (get_val(y-1, x-1) + get_val(y-1, x+1) + get_val(y+1, x-1) + get_val(y+1, x+1)) / 4;
                    g = (get_val(y, x-1) + get_val(y, x+1) + get_val(y-1, x) + get_val(y+1, x)) / 4;
                    r = get_val(y, x);
                }
            }

            // 후처리 모듈(OpenCV)과의 호환을 위해 BGR 순서로 담아줍니다.
            bgr_buf[idx + 0] = b;
            bgr_buf[idx + 1] = g;
            bgr_buf[idx + 2] = r;
        }
    }
    return bgr_buf;
}

// 🌟 3. 브릿지 함수 수정: OpenCV의 cvtColor 완전 제거!
cv::Mat runPureISP(cv::Mat& raw16_frame) {
    if (raw16_frame.empty() || raw16_frame.type() != CV_16UC1) {
        std::cerr << "🚨 입력이 16-bit RAW 데이터가 아닙니다!" << std::endl;
        return raw16_frame;
    }

    int width = raw16_frame.cols;
    int height = raw16_frame.rows;
    uint16_t* raw_data = (uint16_t*)raw16_frame.data;

    // [순수 C++ 1단계] BLC & AWB 엔진 가동 (원본 RAW 버퍼 직접 수정)
    ISPConfig cfg;
    applyInitialISP(raw_data, width, height, cfg);

    // [순수 C++ 2단계] 수동 Demosaicing (Bayer 10-bit -> BGR 8-bit 배열로 복원)
    std::vector<uint8_t> bgr_buffer = applyPureDemosaic(raw_data, width, height);

    // [어플리케이션 계층] 순수 C++로 만든 1차원 배열을 OpenCV의 2차원 객체로 '포장'만 해줍니다.
    cv::Mat bgr_img(height, width, CV_8UC3);
    std::copy(bgr_buffer.begin(), bgr_buffer.end(), bgr_img.data);

    // 이제 순수 C++로 1차 가공된 컬러 사진이 기존의 화질 튜닝 파이프라인으로 넘어갑니다!
    return bgr_img; 
}

// 1. Gamma Correnction + Tone Mapping (기존과 동일)
void applyShadowBoost(const cv::Mat& src, cv::Mat& dst, double gamma, double alpha) {
    if (src.empty()) return;

    cv::Mat yuv;
    cv::cvtColor(src, yuv, cv::COLOR_BGR2YUV); 

    unsigned char lut[256];
    for (int i = 0; i < 256; i++) {
        double I_in = i / 255.0; 
        double I_boost = std::pow(I_in, 1.0 / gamma); 
        I_boost = alpha * (I_boost - 0.5) + 0.5; 
        double shadow_weight = 1.0 - std::pow(I_in, 2.0); 
        double final_I = (I_boost * shadow_weight) + (I_in * (1.0 - shadow_weight));
        lut[i] = cv::saturate_cast<unsigned char>(final_I * 255.0);
    }
    
    int rows = yuv.rows;
    int cols = yuv.cols;
    if (yuv.isContinuous()) { 
        cols *= rows;
        rows = 1;
    }
    
    for (int r = 0; r < rows; r++) {
        cv::Vec3b* ptr = yuv.ptr<cv::Vec3b>(r);
        for (int c = 0; c < cols; c++) {
            ptr[c][0] = lut[ptr[c][0]]; 
        }
    }
    cv::cvtColor(yuv, dst, cv::COLOR_YUV2BGR);
}

// 3. CLAHE (기존과 동일)
void applyCLAHE(const cv::Mat& src, cv::Mat& dst, double clip_limit, cv::Size grid) {
    cv::Mat lab;
    cv::cvtColor(src, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> planes;
    cv::split(lab, planes);
    cv::Mat L = planes[0]; 

    int rows = L.rows;
    int cols = L.cols;
    int grid_x = grid.width;
    int grid_y = grid.height;
    int tile_w = std::ceil((float)cols / grid_x);
    int tile_h = std::ceil((float)rows / grid_y);

    std::vector<std::vector<std::vector<int>>> cdfs(grid_y, std::vector<std::vector<int>>(grid_x, std::vector<int>(256, 0)));
    int clip_threshold = std::max(1, (int)(clip_limit * (tile_w * tile_h) / 256.0));

    for (int ty = 0; ty < grid_y; ty++) {
        for (int tx = 0; tx < grid_x; tx++) {
            int hist[256] = {0};
            int y_start = ty * tile_h;
            int y_end = std::min(rows, y_start + tile_h);
            int x_start = tx * tile_w;
            int x_end = std::min(cols, x_start + tile_w);
            int num_pixels = (y_end - y_start) * (x_end - x_start);

            for (int y = y_start; y < y_end; y++) {
                const uchar* ptr = L.ptr<uchar>(y);
                for (int x = x_start; x < x_end; x++) {
                    hist[ptr[x]]++;
                }
            }

            int clipped_pixels = 0;
            for (int i = 0; i < 256; i++) {
                if (hist[i] > clip_threshold) {
                    clipped_pixels += (hist[i] - clip_threshold);
                    hist[i] = clip_threshold;
                }
            }
            int redist_step = clipped_pixels / 256;
            int redist_residual = clipped_pixels % 256;
            for (int i = 0; i < 256; i++) {
                hist[i] += redist_step;
                if (i < redist_residual) hist[i]++;
            }

            int sum = 0;
            for (int i = 0; i < 256; i++) {
                sum += hist[i];
                cdfs[ty][tx][i] = (sum * 255) / num_pixels;
            }
        }
    }

    cv::Mat out_L = L.clone();
    for (int y = 0; y < rows; y++) {
        uchar* out_ptr = out_L.ptr<uchar>(y);
        const uchar* in_ptr = L.ptr<uchar>(y);

        float ty_f = (float)y / tile_h - 0.5f;
        int ty1 = std::max(0, (int)std::floor(ty_f));
        int ty2 = std::min(grid_y - 1, ty1 + 1);
        float y_ratio = ty_f - ty1;
        if (ty_f < 0) y_ratio = 0;

        for (int x = 0; x < cols; x++) {
            float tx_f = (float)x / tile_w - 0.5f;
            int tx1 = std::max(0, (int)std::floor(tx_f));
            int tx2 = std::min(grid_x - 1, tx1 + 1);
            float x_ratio = tx_f - tx1;
            if (tx_f < 0) x_ratio = 0;

            uchar val = in_ptr[x];
            int cdf11 = cdfs[ty1][tx1][val];
            int cdf12 = cdfs[ty1][tx2][val];
            int cdf21 = cdfs[ty2][tx1][val];
            int cdf22 = cdfs[ty2][tx2][val];

            float interp_top = cdf11 * (1.0f - x_ratio) + cdf12 * x_ratio;
            float interp_bot = cdf21 * (1.0f - x_ratio) + cdf22 * x_ratio;
            float final_val = interp_top * (1.0f - y_ratio) + interp_bot * y_ratio;

            out_ptr[x] = cv::saturate_cast<uchar>(final_val);
        }
    }

    planes[0] = out_L;
    cv::merge(planes, lab);
    cv::cvtColor(lab, dst, cv::COLOR_Lab2BGR);
}

// 엔트로피 계산 함수 (기존과 동일)
double calculateEntropy(const cv::Mat& frame) {
    if (frame.empty()) return 0.0;
    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    int histSize = 256;
    float range[] = { 0, 256 };
    const float* histRange = { range };
    cv::Mat hist;
    cv::calcHist(&gray, 1, 0, cv::Mat(), hist, 1, &histSize, &histRange, true, false);
    hist /= (gray.rows * gray.cols);
    double entropy = 0.0;
    for (int i = 0; i < histSize; i++) {
        float p = hist.at<float>(i);
        if (p > 0.0) entropy -= p * std::log2(p);
    }
    return entropy;
}

// 5. 8분할 이미지 & Bestshot 생성 함수 (기존과 동일)
cv::Mat processISPAndGetBest(const cv::Mat& raw_frame_in, cv::Mat& tuning_view_out) {
    if (raw_frame_in.empty()) return raw_frame_in;

    std::vector<cv::Mat> candidates(8);
    candidates[0] = raw_frame_in.clone();

    cv::Mat t_boost;
    applyShadowBoost(raw_frame_in, t_boost, 1.2, 1.0); applyCLAHE(t_boost, candidates[1], 1.5, cv::Size(8,8));
    applyShadowBoost(raw_frame_in, t_boost, 1.5, 1.2); applyCLAHE(t_boost, candidates[2], 2.0, cv::Size(8,8));
    applyShadowBoost(raw_frame_in, t_boost, 1.8, 1.4); applyCLAHE(t_boost, candidates[3], 2.5, cv::Size(8,8));
    applyShadowBoost(raw_frame_in, t_boost, 2.2, 1.6); applyCLAHE(t_boost, candidates[4], 3.0, cv::Size(8,8));
    applyShadowBoost(raw_frame_in, t_boost, 2.5, 1.8); applyCLAHE(t_boost, candidates[5], 3.5, cv::Size(8,8));
    applyShadowBoost(raw_frame_in, t_boost, 2.8, 2.0); applyCLAHE(t_boost, candidates[6], 4.0, cv::Size(8,8));
    applyShadowBoost(raw_frame_in, t_boost, 3.0, 2.2); applyCLAHE(t_boost, candidates[7], 4.5, cv::Size(8,8));   

    cv::Mat gray_raw;
    cv::cvtColor(raw_frame_in, gray_raw, cv::COLOR_BGR2GRAY);
    cv::Scalar mean_val, stddev_val;
    cv::meanStdDev(gray_raw, mean_val, stddev_val);

    std::string raw_info = cv::format("Mean: %.1f, Std: %.1f", mean_val[0], stddev_val[0]);

    double scale = 1920.0 / raw_frame_in.cols;
    cv::Mat raw_resized;
    cv::resize(raw_frame_in, raw_resized, cv::Size(), scale, scale, cv::INTER_AREA);
    
    int q_rows = raw_resized.rows / 2; 
    int q_cols = raw_resized.cols / 2; 
    tuning_view_out = cv::Mat(q_rows * 2, q_cols * 4, CV_8UC3, cv::Scalar(0,0,0));

    std::vector<std::string> titles = {
        "1. RAW", "2. Processing Lv.1", "3. Processing Lv.2", "4. Processing Lv.3", 
        "5. Processing Lv.4", "6. Processing Lv.5", "7. Processing Lv.6", "8. Processing Lv.7"
    };
    std::vector<std::string> subtitles = {
        raw_info, 
        "(G=1.2, A=1.0)", "(G=1.5, A=1.2)", "(G=1.8, A=1.4)", 
        "(G=2.2, A=1.6)", "(G=2.5, A=1.8)", "(G=2.8, A=2.0)", "(G=3.0, A=2.2)"
    };

    double max_entropy = -1.0;
    int best_idx = 0;
    std::vector<double> entropies(8);

    for (int i = 0; i < 8; i++) {
        entropies[i] = calculateEntropy(candidates[i]); 
        if (entropies[i] > max_entropy) {
            max_entropy = entropies[i];
            best_idx = i;
        }
    }

    for (int i = 0; i < 8; i++) {
        cv::Mat q_resized;
        cv::resize(candidates[i], q_resized, cv::Size(q_cols, q_rows));
        cv::Scalar text_color = (i == best_idx) ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);

        cv::putText(q_resized, titles[i], cv::Point(15, 35), cv::FONT_HERSHEY_SIMPLEX, 0.9, cv::Scalar(0, 0, 0), 2, cv::LINE_AA);
        if (!subtitles[i].empty()) {
            cv::putText(q_resized, subtitles[i], cv::Point(15, 70), cv::FONT_HERSHEY_SIMPLEX, 0.7, text_color, 2, cv::LINE_AA);
        }
        std::string entropy_text = cv::format("Entropy : %.2f", entropies[i]);
        cv::putText(q_resized, entropy_text, cv::Point(15, 105), cv::FONT_HERSHEY_SIMPLEX, 0.8, text_color, 2, cv::LINE_AA);

        int r = i / 4;
        int c = i % 4;
        q_resized.copyTo(tuning_view_out(cv::Rect(c * q_cols, r * q_rows, q_cols, q_rows)));
    }

    return candidates[best_idx];
}