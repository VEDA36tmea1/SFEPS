#include "../inc/img_processing.h"
#include <cmath>
#include <iostream>
#include <vector>
#include <algorithm>
#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/dnn.hpp>

// 1. AGC
void applyShadowBoost(const cv::Mat& src, cv::Mat& dst, double gamma_val, double alpha_val) {
    if (src.empty()) return;
    cv::Mat yuv;
    cv::cvtColor(src, yuv, cv::COLOR_BGR2YUV); 

    unsigned char lut[256];
    for (int i = 0; i < 256; i++) {
        double I_in = i / 255.0; 
        double I_boost = std::pow(I_in, 1.0/gamma_val); 
        I_boost = alpha_val * (I_boost - 0.5) + 0.5; 
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

// 2. denoise
void applyBilateralDenoise(const cv::Mat& src, cv::Mat& dst, int d, double sc, double ss) {
    cv::bilateralFilter(src, dst, d, sc, ss);
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

// 4. Sharpening (3x3 커널)
void applySharpen(const cv::Mat& src, cv::Mat& dst, float strength) {
    dst = src.clone(); 
    int rows = src.rows;
    int cols = src.cols;
    int channels = src.channels();

    float center_w = 1.0f + 4.0f * strength; 
    float edge_w = -strength;             

    for (int r = 1; r < rows - 1; r++) {
        const uchar* prev_row = src.ptr<uchar>(r - 1);
        const uchar* curr_row = src.ptr<uchar>(r);
        const uchar* next_row = src.ptr<uchar>(r + 1);
        uchar* out_row = dst.ptr<uchar>(r);

        for (int c = channels; c < (cols - 1) * channels; c++) {
            float sum = center_w * curr_row[c]
                      + edge_w * prev_row[c]                
                      + edge_w * next_row[c]                
                      + edge_w * curr_row[c - channels]     
                      + edge_w * curr_row[c + channels];    
                      
            out_row[c] = cv::saturate_cast<uchar>(sum);
        }
    }
}

double getPersonConfidence(const cv::Mat& frame, cv::dnn::Net& net) {
    if (net.empty()) return 0.0;
    
    // YOLO는 416x416 해상도, RGB 변환(true), 1/255.0 정규화를 사용합니다.
    cv::Mat blob = cv::dnn::blobFromImage(frame, 1/255.0, cv::Size(416, 416), cv::Scalar(0,0,0), true, false);
    net.setInput(blob);
    
    std::vector<cv::String> outNames = net.getUnconnectedOutLayersNames();
    std::vector<cv::Mat> outs;
    net.forward(outs, outNames);

    double max_person_conf = 0.0;
    
    for (size_t i = 0; i < outs.size(); ++i) {
        float* data = (float*)outs[i].data;
        for (int j = 0; j < outs[i].rows; ++j, data += outs[i].cols) {
            cv::Mat scores = outs[i].row(j).colRange(5, outs[i].cols);
            cv::Point classIdPoint;
            double confidence;
            cv::minMaxLoc(scores, 0, &confidence, 0, &classIdPoint);
            
            // COCO 데이터셋 기준, 0번 클래스가 '사람(Person)' 입니다.
            if (classIdPoint.x == 0 && confidence > max_person_conf) {
                max_person_conf = confidence;
            }
        }
    }
    return max_person_conf;
}

// 5. 8분할 이미지 생성
void createTuningView(const cv::Mat& raw_frame_in, cv::Mat& tuning_view, cv::dnn::Net& net) {
    if (raw_frame_in.empty()) return;

    // 해상도 정규화
    cv::Mat raw_frame;
    double scale = 1920.0 / raw_frame_in.cols;
    cv::resize(raw_frame_in, raw_frame, cv::Size(), scale, scale, cv::INTER_AREA);
    
    int rows = raw_frame.rows;
    int cols = raw_frame.cols;
    int q_rows = rows / 2; 
    int q_cols = cols / 2; 

    tuning_view = cv::Mat(q_rows * 2, q_cols * 4, CV_8UC3, cv::Scalar(0,0,0));
    std::vector<cv::Mat> results(8);

    // Q1: 원본
    results[0] = raw_frame.clone();

    // Q2: ONLY AGC
    applyShadowBoost(raw_frame, results[1], 2.2, 1.2); 

    // Q3: ONLY CLAHE
    applyCLAHE(raw_frame, results[2], 4.0, cv::Size(8,8)); 

    // Q4 ~ Q7: AGC + CLAHE 차등 적용
    cv::Mat tmp4, tmp5, tmp6, tmp7;

    applyShadowBoost(raw_frame, tmp4, 1.2, 1.1); 
    applyCLAHE(tmp4, results[3], 1.5, cv::Size(8,8));

    applyShadowBoost(raw_frame, tmp5, 1.5, 1.2); 
    applyCLAHE(tmp5, results[4], 2.0, cv::Size(8,8));

    applyShadowBoost(raw_frame, tmp6, 1.8, 1.4); 
    applyCLAHE(tmp6, results[5], 2.5, cv::Size(8,8));

    applyShadowBoost(raw_frame, tmp7, 2.5, 1.8); 
    applyCLAHE(tmp7, results[6], 4.0, cv::Size(8,8));

    cv::Mat tmp8_agc, tmp8_dn, tmp8_cl;
    applyShadowBoost(raw_frame, tmp8_agc, 1.2, 1.1);       
    applyBilateralDenoise(tmp8_agc, tmp8_dn, 9, 75, 75);   
    applyCLAHE(tmp8_dn, tmp8_cl, 1.5, cv::Size(8,8));      
    applySharpen(tmp8_cl, results[7], 1.5);   

    std::vector<std::string> titles = {
        "1. RAW", "2. ONLY AGC", "3. ONLY CLAHE", "4. AGC + CLAHE", 
        "5. AGC + CLAHE", "6. AGC + CLAHE", "7. AGC + CLAHE", "8. Final"
    };

    std::vector<std::string> subtitles = {
        "", "(G=2.2, A=1.2)", "(Clip=4.0)", "(G=1.2, A=1.1, C=1.5)", 
        "(G=1.5, A=1.2, C=2.0)", "(G=1.8, A=1.4, C=2.5)", "(G=2.5, A=1.8, C=4.0)", "(AGC->DN->CLAHE->SHRP)"
    };

    int q_idx = 0;
    for (int r = 0; r < 2; r++) { 
        for (int c = 0; c < 4; c++) {
            cv::Mat q_resized;
            cv::resize(results[q_idx].clone(), q_resized, cv::Size(q_cols, q_rows));

            double conf = getPersonConfidence(results[q_idx], net);
            std::string conf_text = cv::format("AI Confidence: %.1f%%", conf * 100.0);

            // 노란색으로 제목 출력 (잘 보이게 색상 변경)
            cv::putText(q_resized, titles[q_idx], cv::Point(15, 35), 
                        cv::FONT_HERSHEY_SIMPLEX, 0.9, cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
                        
            // AI 신뢰도를 초록색(50% 이상) 또는 빨간색(미만)으로 출력
            cv::Scalar color = (conf > 0.5) ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);
            cv::putText(q_resized, conf_text, cv::Point(15, 75), 
                        cv::FONT_HERSHEY_SIMPLEX, 0.8, color, 2, cv::LINE_AA);

            q_resized.copyTo(tuning_view(cv::Rect(c * q_cols, r * q_rows, q_cols, q_rows)));
            q_idx++;
        }
    }
}