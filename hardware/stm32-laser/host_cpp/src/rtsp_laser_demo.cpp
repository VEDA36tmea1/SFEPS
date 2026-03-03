#include "vision_detector.h"
#include "target_provider.h"

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <iostream>
#include <memory>
#include <string>

int main(int argc, char** argv)
{
    std::string uri = "rtsp://admin:CCgbdCCgbd@192.168.0.22/profile2/media.smp";
    if (argc > 1)
        uri = argv[1];

    std::cout << "[rtsp_laser_demo] open: " << uri << std::endl;

    cv::VideoCapture cap(uri);
    if (!cap.isOpened())
    {
        std::cerr << "[rtsp_laser_demo] failed to open RTSP stream\n";
        return 1;
    }

    VisionDetector detector;

    cv::namedWindow("rtsp_laser_demo");
    // 타겟 ROI 제공: 마우스로 박스 생성/이동. 향후 PersonDetectorTargetProvider로 교체 가능.
    std::unique_ptr<ITargetProvider> targetProvider =
        std::make_unique<InteractiveBoxTargetProvider>(10, 10);
    if (auto* wa = dynamic_cast<IWindowAttachable*>(targetProvider.get()))
    {
        wa->attachToWindow("rtsp_laser_demo");
    }

    cv::Mat frame;
    int frame_id = 0;
    std::cout << "[rtsp_laser_demo] 빈 곳 드래그: 박스 생성(가변 크기), 박스 안 드래그: 박스 이동\n";

    while (true)
    {
        if (!cap.read(frame) || frame.empty())
        {
            std::cerr << "[rtsp_laser_demo] empty frame\n";
            break;
        }

        ++frame_id;

        // 타겟 ROI (마우스 드래그로 지정된 영역)
        TargetROI targetROI = targetProvider->getTarget(frame);
        if (targetROI.valid)
        {
            cv::rectangle(frame, targetROI.rect, cv::Scalar(0, 255, 0), 2);
            cv::circle(frame, targetROI.center(), 3, cv::Scalar(0, 255, 0), -1);
        }

        // 레이저 검출
        DetectionResult laser = detector.detectLaser(frame);
        if (laser.found)
        {
            std::cout << "[rtsp_laser_demo] frame " << frame_id
                      << " laser=(" << laser.point.x << ", " << laser.point.y << ")\n";
            cv::circle(frame, laser.point, 5, cv::Scalar(0, 0, 255), -1);
        }

        cv::imshow("rtsp_laser_demo", frame);
        int key = cv::waitKey(1);
        if (key == 27 || key == 'q')
            break;
    }

    return 0;
}
