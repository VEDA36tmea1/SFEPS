#include "vision_detector.h"

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    std::string uri = "rtsp://admin:CCgbdCCgbd@192.168.0.11/profile2/media.smp";
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

    cv::Mat frame;
    int frame_id = 0;

    while (true)
    {
        if (!cap.read(frame) || frame.empty())
        {
            std::cerr << "[rtsp_laser_demo] empty frame\n";
            break;
        }

        ++frame_id;

        DetectionResult laser = detector.detectLaser(frame);
        if (laser.found)
        {
            // 좌표 로그
            std::cout << "[rtsp_laser_demo] frame " << frame_id
                      << " laser=(" << laser.point.x << ", " << laser.point.y << ")\n";

            // 시각화: 빨간 점 표시
            cv::circle(frame, laser.point, 5, cv::Scalar(0, 0, 255), -1);
        }

        cv::imshow("rtsp_laser_demo", frame);
        int key = cv::waitKey(1);
        if (key == 27 || key == 'q')  // ESC or q
            break;
    }

    return 0;
}

