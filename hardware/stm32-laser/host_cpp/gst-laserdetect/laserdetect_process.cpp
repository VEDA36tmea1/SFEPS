#include "laserdetect_process.h"
#include "vision_detector.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

static VisionDetector s_detector;

void laserdetect_process(unsigned char* data, int width, int height, int row_stride)
{
    if (!data || width <= 0 || height <= 0)
        return;
    if (row_stride < width * 3)
        row_stride = width * 3;

    cv::Mat frame(height, width, CV_8UC3, data, static_cast<size_t>(row_stride));
    DetectionResult result = s_detector.detectLaser(frame);
    if (result.found)
        cv::circle(frame, result.point, 5, cv::Scalar(0, 0, 255), -1);
}
