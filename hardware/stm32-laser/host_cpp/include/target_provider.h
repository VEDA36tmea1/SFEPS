#pragma once

#include <opencv2/core.hpp>
#include <string>

/// 타겟(추적 대상) ROI. 바운딩 박스 형태.
/// 사람 검출 등으로 확장 시 동일 구조 사용.
struct TargetROI
{
    cv::Rect rect;   ///< 바운딩 박스 (x, y, width, height)
    bool valid{false};

    cv::Point2f center() const
    {
        return cv::Point2f(rect.x + rect.width * 0.5f,
                          rect.y + rect.height * 0.5f);
    }
};

/// 타겟 ROI를 제공하는 인터페이스.
/// MouseDragTargetProvider(현재) 또는 PersonDetectorTargetProvider(향후) 구현.
class ITargetProvider
{
public:
    virtual ~ITargetProvider() = default;
    virtual TargetROI getTarget(const cv::Mat& frame) = 0;
};

/// (선택) UI 윈도우에 마우스 콜백을 붙일 수 있는 Provider 인터페이스.
/// 사람 검출 Provider 같은 비-UI 구현체는 이 인터페이스를 구현하지 않아도 됨.
class IWindowAttachable
{
public:
    virtual ~IWindowAttachable() = default;
    virtual void attachToWindow(const std::string& windowName) = 0;
};

/// 마우스 드래그로 고정 크기 ROI의 중심을 지정.
/// 클릭 후 드래그하면 박스 중심이 마우스를 따라감.
class MouseDragTargetProvider : public ITargetProvider, public IWindowAttachable
{
public:
    MouseDragTargetProvider(int boxWidth = 120, int boxHeight = 120);
    ~MouseDragTargetProvider() override = default;

    /// 지정한 윈도우에 마우스 콜백 등록. getTarget() 호출 전에 한 번 호출.
    void attachToWindow(const std::string& windowName) override;

    TargetROI getTarget(const cv::Mat& frame) override;

private:
    int boxWidth_;
    int boxHeight_;
    std::string windowName_;
    float centerX_{0.f};
    float centerY_{0.f};
    bool valid_{false};
    bool dragging_{false};

    static void onMouse(int event, int x, int y, int flags, void* userdata);
};

/// 마우스로 “원하는 크기” ROI 박스를 생성/이동하는 Provider.
///
/// - 빈 곳에서 마우스 좌클릭 드래그: 드래그 영역으로 ROI 생성(가변 크기)
/// - 생성된 박스 안에서 좌클릭 드래그: 박스 이동
class InteractiveBoxTargetProvider : public ITargetProvider, public IWindowAttachable
{
public:
    InteractiveBoxTargetProvider(int minWidth = 10, int minHeight = 10);
    ~InteractiveBoxTargetProvider() override = default;

    void attachToWindow(const std::string& windowName) override;
    TargetROI getTarget(const cv::Mat& frame) override;

private:
    std::string windowName_;
    cv::Rect rect_{};
    bool valid_{false};

    bool creating_{false};
    bool moving_{false};
    cv::Point anchor_{0, 0};
    cv::Point moveOffset_{0, 0};

    int frameW_{0};
    int frameH_{0};

    int minW_;
    int minH_;

    static void onMouse(int event, int x, int y, int flags, void* userdata);
    void clampRectToFrame();
};
