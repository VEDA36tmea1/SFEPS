#include "vision_detector.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

#include <iostream>

// NOTE:
// 현재 구현은 데모용 레이저/타겟 검출 예제이다.
// - 타겟: 프레임 중앙 근처의 고정 위치로 가정
// - 레이저: HSV 색 공간에서 "빨간색" 범위를 마스크링한 뒤,
//           컨투어들의 최소 외접원을 구해 가장 큰 붉은 원형 스폿의 중심을 선택.
//           색 기반으로 실패하면 마지막 fallback 으로 GRAY에서 가장 밝은 픽셀을 사용.
// 실제 프로젝트에서는 카메라/레이저 스펙에 맞게 HSV 범위, 모폴로지, contour 필터 등을
// 튜닝하거나, 필요 시 딥러닝 detector 등으로 교체해야 한다.

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

    // 1) BGR → HSV 변환
    cv::Mat hsv;
    if (frame.channels() == 3)
    {
        cv::cvtColor(frame, hsv, cv::COLOR_BGR2HSV);
    }
    else
    {
        // 단일 채널이면 컬러 정보를 잃었으므로 기존 밝기 기반 fallback 사용
        cv::Mat gray = frame;
        double minVal = 0.0, maxVal = 0.0;
        cv::Point minLoc, maxLoc;
        cv::minMaxLoc(gray, &minVal, &maxVal, &minLoc, &maxLoc);
        if (maxVal > 50.0)
        {
            result.point = cv::Point2f(static_cast<float>(maxLoc.x),
                                       static_cast<float>(maxLoc.y));
            result.found = true;
        }
        return result;
    }

    // 2) 빨간색 범위 HSV threshold (두 구간: 0~10, 170~180)
    cv::Mat mask1, mask2, mask;
    // Hue: [0,10] or [170,180], S/V 꽤 높게 설정 (경험적으로 조정)
    // S(채도)를 50~70 수준으로, V(명도)를 100 수준으로 대폭 낮춰보세요.
    cv::inRange(hsv, cv::Scalar(0, 50, 100), cv::Scalar(10, 255, 255), mask1);
    cv::inRange(hsv, cv::Scalar(160, 50, 100), cv::Scalar(180, 255, 255), mask2);
    cv::bitwise_or(mask1, mask2, mask);

    //cv::imshow("org_mask", mask);

    // 3) 노이즈 제거 (블러 + 모폴로지)
    cv::GaussianBlur(mask, mask, cv::Size(5, 5), 0);
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
    cv::morphologyEx(mask, mask, cv::MORPH_OPEN, kernel);
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel);

    // 4) 디버그용: 빨간색 마스크 영상 직접 보기
    //    - 밝은 영역이 레이저로 마스킹된 부분
    //cv::imshow("laser_mask", mask);
    //cv::waitKey(1);

    // 5) (임시 구현) 마스크에서 가장 밝은 픽셀을 레이저 중심으로 사용
    double minVal = 0.0, maxVal = 0.0;
    cv::Point minLoc, maxLoc;
    cv::minMaxLoc(mask, &minVal, &maxVal, &minLoc, &maxLoc);
    if (maxVal > 50.0)  // 마스크 값 0~255 기준, 임계값은 경험적으로 조정
    {
        result.point = cv::Point2f(static_cast<float>(maxLoc.x),
                                   static_cast<float>(maxLoc.y));
        result.found = true;

        // 포인트 영역의 색상(BGR, HSV) 디버그 출력 (stderr로만 보냄 – 파이프라인 stdout에는 영향 없음)
        // 단, 레이저 위치가 충분히 변했을 때만 출력해서 로그 스팸을 줄인다.
        static cv::Point2f prev_point(-1.f, -1.f);
        const float thresh2 = 4.0f * 4.0f; // 4픽셀 이상 이동했을 때만 (거리^2 기준)

        float dx = result.point.x - prev_point.x;
        float dy = result.point.y - prev_point.y;
        float dist2 = dx * dx + dy * dy;

        if (prev_point.x < 0.f || prev_point.y < 0.f || dist2 > thresh2)
        {
            int px = static_cast<int>(result.point.x + 0.5f);
            int py = static_cast<int>(result.point.y + 0.5f);
            if (px >= 0 && px < frame.cols && py >= 0 && py < frame.rows)
            {
                cv::Vec3b bgr = frame.at<cv::Vec3b>(py, px);
                cv::Vec3b hsv_val = hsv.at<cv::Vec3b>(py, px);
                std::cerr << "[laser point] x=" << result.point.x << " y=" << result.point.y
                          << " BGR=(" << (int)bgr[0] << "," << (int)bgr[1] << "," << (int)bgr[2] << ")"
                          << " HSV=(" << (int)hsv_val[0] << "," << (int)hsv_val[1] << "," << (int)hsv_val[2] << ")\n";
            }
            prev_point = result.point;
        }
    }

    return result;
}

