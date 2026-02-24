#include "vision_detector.h"

#include <opencv2/imgproc.hpp>

// NOTE:
// 현재 구현은 아주 단순한 스텁(stub) 예제이다.
// - 타겟: 프레임 중앙 근처의 고정 위치로 가정
// - 레이저: 가장 밝은 픽셀을 찾는 방식의 간단한 예시
// 실제 프로젝트에서는 색/threshold, contour, 딥러닝 detector 등으로 교체해야 한다.

DetectionResult VisionDetector::detectTarget(const cv::Mat& frame)
{
    DetectionResult result;
    if (frame.empty())
        return result;

    // 예제: 이미지 중앙을 타겟으로 가정
    result.point = cv::Point2f(frame.cols * 0.5f, frame.rows * 0.5f);
    result.found = true;
    return result;
}

DetectionResult VisionDetector::detectLaser(const cv::Mat& frame)
{
    DetectionResult result;
    if (frame.empty())
        return result;

    cv::Mat gray;
    if (frame.channels() == 3)
    {
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    }
    else
    {
        gray = frame;
    }

    double minVal = 0.0, maxVal = 0.0;
    cv::Point minLoc, maxLoc;
    cv::minMaxLoc(gray, &minVal, &maxVal, &minLoc, &maxLoc);

    // 최대 밝기 픽셀을 레이저 스폿으로 가정
    result.point = cv::Point2f(static_cast<float>(maxLoc.x),
                               static_cast<float>(maxLoc.y));
    // 간단하게, 어느 정도 이상 밝으면 found 로 본다 (임계값은 경험적으로 조정)
    result.found = (maxVal > 50.0);
    return result;
}

