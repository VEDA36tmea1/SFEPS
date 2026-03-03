#ifndef IMG_PROCESSING_H
#define IMG_PROCESSING_H

#include <opencv2/opencv.hpp>
#include <vector>

// 1. AGC
void applyShadowBoost(const cv::Mat& src, cv::Mat& dst, double gamma_val = 1.0, double alpha_val = 1.0);

// 2. denoise
void applyBilateralDenoise(const cv::Mat& src, cv::Mat& dst, int d=9, double sc=75, double ss=75);

// 3. CLAHE
void applyCLAHE(const cv::Mat& src, cv::Mat& dst, double clip=1.5, cv::Size grid=cv::Size(8,8));

// 4. Sharpening
void applySharpen(const cv::Mat& src, cv::Mat& dst, float strength=1.0);

// 5. 8분할 이미지 생성 함수
void createTuningView(const cv::Mat& raw_frame_in, cv::Mat& tuning_view);

#endif