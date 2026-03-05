#ifndef IMG_PROCESSING_H
#define IMG_PROCESSING_H

#include <opencv2/opencv.hpp>
#include <vector>

// 1. AGC
void applyShadowBoost(const cv::Mat& src, cv::Mat& dst, double gamma_val = 1.0, double alpha_val = 1.0);

// 2. CLAHE
void applyCLAHE(const cv::Mat& src, cv::Mat& dst, double clip=1.5, cv::Size grid=cv::Size(8,8));

// 3. 8분할 이미지 & Bestshot 생성 함수
cv::Mat processISPAndGetBest(const cv::Mat& raw_frame_in, cv::Mat& tuning_view_out);;

// 4. 엔트로피 계산 함수
double calculateEntropy(const cv::Mat& frame);
#endif