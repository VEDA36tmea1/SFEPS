#include "target_provider.h"
#include <opencv2/highgui.hpp>
#include <algorithm>

MouseDragTargetProvider::MouseDragTargetProvider(int boxWidth, int boxHeight)
    : boxWidth_(boxWidth > 0 ? boxWidth : 120)
    , boxHeight_(boxHeight > 0 ? boxHeight : 120)
{
}

void MouseDragTargetProvider::attachToWindow(const std::string& windowName)
{
    windowName_ = windowName;
    cv::setMouseCallback(windowName, onMouse, this);
}

TargetROI MouseDragTargetProvider::getTarget(const cv::Mat& frame)
{
    TargetROI roi;
    if (!valid_ || frame.empty())
        return roi;

    int cols = frame.cols;
    int rows = frame.rows;

    int halfW = boxWidth_ / 2;
    int halfH = boxHeight_ / 2;

    int x1 = static_cast<int>(centerX_ - halfW);
    int y1 = static_cast<int>(centerY_ - halfH);
    int x2 = x1 + boxWidth_;
    int y2 = y1 + boxHeight_;

    x1 = std::max(0, std::min(x1, cols - 1));
    y1 = std::max(0, std::min(y1, rows - 1));
    x2 = std::max(0, std::min(x2, cols));
    y2 = std::max(0, std::min(y2, rows));

    roi.rect = cv::Rect(x1, y1, x2 - x1, y2 - y1);
    roi.valid = (roi.rect.width > 0 && roi.rect.height > 0);
    return roi;
}

void MouseDragTargetProvider::onMouse(int event, int x, int y, int flags, void* userdata)
{
    auto* self = static_cast<MouseDragTargetProvider*>(userdata);
    if (!self)
        return;

    if (event == cv::EVENT_LBUTTONDOWN)
    {
        self->centerX_ = static_cast<float>(x);
        self->centerY_ = static_cast<float>(y);
        self->valid_ = true;
        self->dragging_ = true;
    }
    else if (event == cv::EVENT_MOUSEMOVE && (flags & cv::EVENT_FLAG_LBUTTON))
    {
        if (self->dragging_)
        {
            self->centerX_ = static_cast<float>(x);
            self->centerY_ = static_cast<float>(y);
        }
    }
    else if (event == cv::EVENT_LBUTTONUP)
    {
        self->dragging_ = false;
    }
}
