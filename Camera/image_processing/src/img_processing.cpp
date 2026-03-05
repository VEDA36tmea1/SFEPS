#include "../inc/img_processing.h"
#include <cmath>
#include <iostream>
#include <vector>
#include <algorithm>
#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/dnn.hpp>

// 1. Gamma Correnction + Tone Mapping
void applyShadowBoost(const cv::Mat& src, cv::Mat& dst, double gamma, double alpha) {
    if (src.empty()) return;

    // 색상 공간 변환
    cv::Mat yuv;
    cv::cvtColor(src, yuv, cv::COLOR_BGR2YUV); 

    // 룩업 테이블 생성
    unsigned char lut[256];
    for (int i = 0; i < 256; i++) {
        double I_in = i / 255.0; // 정규화
        double I_boost = std::pow(I_in, 1.0 / gamma); // 감마 보정 
        I_boost = alpha * (I_boost - 0.5) + 0.5; // 대비 조절
        double shadow_weight = 1.0 - std::pow(I_in, 2.0); // 암부 가중치 계산
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


// 3. CLAHE
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

    // 히스토그램 생성 및 클리핑
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

    // 이중 선형 보간법 매핑
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

// 엔트로피 계산 함수
double calculateEntropy(const cv::Mat& frame) {
    if (frame.empty()) return 0.0;

    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

    // 1. 히스토그램 계산
    int histSize = 256;
    float range[] = { 0, 256 };
    const float* histRange = { range };
    cv::Mat hist;
    cv::calcHist(&gray, 1, 0, cv::Mat(), hist, 1, &histSize, &histRange, true, false);

    // 2. 전체 픽셀 수로 나누어 확률 p(i) 계산
    hist /= (gray.rows * gray.cols);

    // 3. 섀넌 엔트로피 공식 적용: -sum( p * log2(p) )
    double entropy = 0.0;
    for (int i = 0; i < histSize; i++) {
        float p = hist.at<float>(i);
        if (p > 0.0) {
            entropy -= p * std::log2(p);
        }
    }
    return entropy;
}

// 5. 8분할 이미지 & Bestshot 생성 함수
cv::Mat processISPAndGetBest(const cv::Mat& raw_frame_in, cv::Mat& tuning_view_out) {
    if (raw_frame_in.empty()) return raw_frame_in;

    // 1. FHD 원본 해상도로 8가지 파이프라인 모두 생성
    std::vector<cv::Mat> candidates(8);
    candidates[0] = raw_frame_in.clone(); // 원본

    cv::Mat t_boost;

    // Lv.1
    applyShadowBoost(raw_frame_in, t_boost, 1.2, 1.0); 
    applyCLAHE(t_boost, candidates[1], 1.5, cv::Size(8,8));

    // Lv.2
    applyShadowBoost(raw_frame_in, t_boost, 1.5, 1.2); 
    applyCLAHE(t_boost, candidates[2], 2.0, cv::Size(8,8));

    // Lv.3
    applyShadowBoost(raw_frame_in, t_boost, 1.8, 1.4); 
    applyCLAHE(t_boost, candidates[3], 2.5, cv::Size(8,8));

    // Lv.4
    applyShadowBoost(raw_frame_in, t_boost, 2.2, 1.6); 
    applyCLAHE(t_boost, candidates[4], 3.0, cv::Size(8,8));

    // Lv.5
    applyShadowBoost(raw_frame_in, t_boost, 2.5, 1.8); 
    applyCLAHE(t_boost, candidates[5], 3.5, cv::Size(8,8));

    // Lv.6
    applyShadowBoost(raw_frame_in, t_boost, 2.8, 2.0); 
    applyCLAHE(t_boost, candidates[6], 4.0, cv::Size(8,8));

    // Lv.7
    applyShadowBoost(raw_frame_in, t_boost, 3.0, 2.2); 
    applyCLAHE(t_boost, candidates[7], 4.5, cv::Size(8,8));   

    cv::Mat gray_raw;
    cv::cvtColor(raw_frame_in, gray_raw, cv::COLOR_BGR2GRAY);
    cv::Scalar mean_val, stddev_val;
    cv::meanStdDev(gray_raw, mean_val, stddev_val);

    std::string raw_info = cv::format("Mean(B): %.1f, Std(C): %.1f", mean_val[0], stddev_val[0]);

    // 2. 실시간 1등 찾기 및 8분할 뷰어(tuning_view_out) 제작을 위한 해상도 축소
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
        "(G=1.2, A=1.0, C=1.5)", "(G=1.5, A=1.2, C=2.0)", "(G=1.8, A=1.4, C=2.5)", 
        "(G=2.2, A=1.6, C=3.0)", "(G=2.5, A=1.8, C=3.5)", "(G=2.8, A=2.0, C=4.0)", "(G=3.0, A=2.2, C=4.5)"
    };

double max_entropy = -1.0;
    int best_idx = 0;
    std::vector<double> entropies(8);

    std::cout << "\n==========================================" << std::endl;
    std::cout << "[Entropy 지표]" << std::endl;
    std::cout << "원본 평균 밝기 : " << mean_val[0] << ", 표준편차(대비) : " << stddev_val[0] << std::endl;
    std::cout << "------------------------------------------" << std::endl;

    for (int i = 0; i < 8; i++) {
        entropies[i] = calculateEntropy(candidates[i]); 
        
        std::string cmd_title = titles[i] + " " + subtitles[i];;
        std::cout << cmd_title << " : " << entropies[i] << std::endl;
        
        if (entropies[i] > max_entropy) {
            max_entropy = entropies[i];
            best_idx = i;
        }
    }
    std::cout << "==========================================\n" << std::endl;
    std::cout << titles[best_idx] << "가 BestShot으로 선정되었습니다." << "\n" << std::endl;

    for (int i = 0; i < 8; i++) {
        // 8분할 이미지 화면 조립을 위해 축소
        cv::Mat q_resized;
        cv::resize(candidates[i], q_resized, cv::Size(q_cols, q_rows));

        // 1등은 초록색, 나머지는 빨간색
        cv::Scalar text_color = (i == best_idx) ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);

        // 1. Draw Title (titles[i] 사용, y=35)
        cv::putText(q_resized, titles[i], cv::Point(15, 35), 
                    cv::FONT_HERSHEY_SIMPLEX, 0.9, cv::Scalar(0, 0, 0), 2, cv::LINE_AA);
        
        // 2. Draw Subtitle (subtitles[i] 사용, y=70, Clip Limit 파라미터 등 표시)
        if (!subtitles[i].empty()) {
            cv::putText(q_resized, subtitles[i], cv::Point(15, 70), 
                        cv::FONT_HERSHEY_SIMPLEX, 0.7, text_color, 2, cv::LINE_AA);
        }

        // 3. Draw Entropy value (y=105)
        std::string entropy_text = cv::format("Entropy : %.2f", entropies[i]);
        cv::putText(q_resized, entropy_text, cv::Point(15, 105), 
                    cv::FONT_HERSHEY_SIMPLEX, 0.8, text_color, 2, cv::LINE_AA);

        //Place quadrant in view
        int r = i / 4;
        int c = i % 4;
        q_resized.copyTo(tuning_view_out(cv::Rect(c * q_cols, r * q_rows, q_cols, q_rows)));
    }

    return candidates[best_idx];
}