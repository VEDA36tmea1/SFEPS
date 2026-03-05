#include "target_provider.h"

#include <opencv2/highgui.hpp>

#include <algorithm>

InteractiveBoxTargetProvider::InteractiveBoxTargetProvider(int minWidth, int minHeight)
    : minW_(std::max(1, minWidth))
    , minH_(std::max(1, minHeight))
{
}

void InteractiveBoxTargetProvider::attachToWindow(const std::string& windowName)
{
    windowName_ = windowName;
    cv::setMouseCallback(windowName, onMouse, this);
}

TargetROI InteractiveBoxTargetProvider::getTarget(const cv::Mat& frame)
{
    TargetROI roi;
    if (frame.empty())
        return roi;

    frameW_ = frame.cols;
    frameH_ = frame.rows;

    if (!valid_)
        return roi;

    clampRectToFrame();
    roi.rect = rect_;
    roi.valid = (rect_.width > 0 && rect_.height > 0);
    return roi;
}

void InteractiveBoxTargetProvider::clampRectToFrame()
{
    if (frameW_ <= 0 || frameH_ <= 0)
        return;

    int x = rect_.x;
    int y = rect_.y;
    int w = rect_.width;
    int h = rect_.height;

    if (w < 0) w = 0;
    if (h < 0) h = 0;

    x = std::max(0, std::min(x, frameW_ - 1));
    y = std::max(0, std::min(y, frameH_ - 1));

    if (x + w > frameW_) w = frameW_ - x;
    if (y + h > frameH_) h = frameH_ - y;

    rect_ = cv::Rect(x, y, std::max(0, w), std::max(0, h));
}

void InteractiveBoxTargetProvider::onMouse(int event, int x, int y, int flags, void* userdata)
{
    (void)flags;
    auto* self = static_cast<InteractiveBoxTargetProvider*>(userdata);
    if (!self)
        return;

    const cv::Point p(x, y);

    switch (event)
    {
    case cv::EVENT_LBUTTONDOWN:
    {
        if (self->valid_ && self->rect_.contains(p))
        {
            // 박스 이동 시작
            self->moving_ = true;
            self->creating_ = false;
            self->moveOffset_ = p - self->rect_.tl();
        }
        else
        {
            // 빈 곳에서 클릭: 새 박스 생성 시작 (또는 기존 박스 클리어)
            self->creating_ = true;
            self->moving_ = false;
            self->anchor_ = p;
            self->rect_ = cv::Rect(p.x, p.y, 0, 0);
            // 실제 드래그(폭/높이 >= minW_/minH_)가 완료되기 전까지는
            // ROI 를 유효하지 않게 유지한다. (빈 곳 "클릭"만 하면 곧바로 사라지도록)
            self->valid_ = false;
        }
        break;
    }
    case cv::EVENT_MOUSEMOVE:
    {
        if (self->creating_)
        {
            int x1 = std::min(self->anchor_.x, p.x);
            int y1 = std::min(self->anchor_.y, p.y);
            int x2 = std::max(self->anchor_.x, p.x);
            int y2 = std::max(self->anchor_.y, p.y);
            self->rect_ = cv::Rect(x1, y1, x2 - x1, y2 - y1);
            self->clampRectToFrame();
        }
        else if (self->moving_)
        {
            // 현재 포인터 위치를 박스 내부의 같은 상대 위치로 유지
            const cv::Point newTl = p - self->moveOffset_;
            self->rect_.x = newTl.x;
            self->rect_.y = newTl.y;
            self->clampRectToFrame();
        }
        break;
    }
    case cv::EVENT_LBUTTONUP:
    {
        if (self->creating_)
        {
            self->creating_ = false;
            self->clampRectToFrame();
            if (self->rect_.width < self->minW_ || self->rect_.height < self->minH_)
            {
                // 너무 작은 드래그는 무시(실수 클릭)
                self->valid_ = false;
                self->rect_ = cv::Rect();
            }
            else
            {
                // 충분히 드래그된 경우에만 새로운 ROI 를 유효로 설정
                self->valid_ = true;
            }
        }
        if (self->moving_)
        {
            self->moving_ = false;
            self->clampRectToFrame();
        }
        break;
    }
    default:
        break;
    }
}

