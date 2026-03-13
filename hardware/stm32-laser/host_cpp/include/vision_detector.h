#pragma once

#include <opencv2/core.hpp>

struct DetectionResult
{
    cv::Point2f point;
    bool found{false};
};

class VisionDetector
{
public:
    // 카메라에서 얻은 프레임을 입력으로 받아 타겟/레이저 중심을 추정한다.
    DetectionResult detectTarget(const cv::Mat& frame);
    // out_mask가 제공되면, 빨간색 마스크(0/255)를 채운다. (디버그/시각화용)
    DetectionResult detectLaser(const cv::Mat& frame, cv::Mat* out_mask = nullptr);
};