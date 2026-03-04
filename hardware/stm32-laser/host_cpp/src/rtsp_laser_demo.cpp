#include "vision_detector.h"
#include "target_provider.h"
// STM32 쪽으로 PID 제어를 옮기고, 호스트에서는 픽셀 오차만 전송한다.
// 필요하면 호스트 측 P 제어 디버깅을 위해 ibvs_controller 를 다시 사용할 수 있다.
#include "ibvs_controller.h"

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <chrono>

int main(int argc, char** argv)
{
    std::string uri = "rtsp://admin:CCgbdCCgbd@192.168.0.22/profile2/media.smp";
    if (argc > 1)
        uri = argv[1];

    // 로그는 stderr로 보내고, stdout은 파이프용 명령 출력에만 사용한다.
    std::cerr << "[rtsp_laser_demo] open: " << uri << std::endl;

    cv::VideoCapture cap(uri);
    if (!cap.isOpened())
    {
        std::cerr << "[rtsp_laser_demo] failed to open RTSP stream\n";
        return 1;
    }

    VisionDetector detector;

    // (옵션) 호스트 측 P 제어기 – 현재는 디버깅용으로만 사용 가능
    IbvsController controller(800, 2200, 1500, 0.02, -0.02);

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
    std::cerr << "[rtsp_laser_demo] 빈 곳 드래그: 박스 생성(가변 크기), 박스 안 드래그: 박스 이동\n";

    // 타이밍 측정용(현재 P 제어에서는 dt를 사용하지 않지만, 인터페이스 유지)
    auto last_time = std::chrono::steady_clock::now();

    // 레이저 좌표 변화에 대해서만 로그를 찍기 위한 이전 레이저 위치
    cv::Point2f prev_laser(-1.f, -1.f);

    while (true)
    {
        if (!cap.read(frame) || frame.empty())
        {
            std::cerr << "[rtsp_laser_demo] empty frame\n";
            break;
        }

        auto now = std::chrono::steady_clock::now();
        double dt_sec =
            std::chrono::duration_cast<std::chrono::duration<double>>(now - last_time).count();
        last_time = now;

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
            // 레이저 좌표 변화가 일정 임계값 이상일 때만 로그 출력
            const float thresh2 = 4.0f * 4.0f; // 4픽셀 이상 이동 시
            float dx = laser.point.x - prev_laser.x;
            float dy = laser.point.y - prev_laser.y;
            float dist2 = dx * dx + dy * dy;

            if (prev_laser.x < 0.f || prev_laser.y < 0.f || dist2 > thresh2)
            {
                std::cerr << "[rtsp_laser_demo] frame " << frame_id
                          << " laser=(" << laser.point.x << ", " << laser.point.y << ")\n";
                prev_laser = laser.point;
            }

            cv::circle(frame, laser.point, 5, cv::Scalar(0, 0, 255), -1);
        }

        // 박스와 레이저가 모두 유효하면, 픽셀 오차(e_u, e_v)를 계산
        if (targetROI.valid && laser.found)
        {
            // 에러: 타겟 - 레이저 (픽셀 단위)
            double e_u = static_cast<double>(targetROI.center().x - laser.point.x);
            double e_v = static_cast<double>(targetROI.center().y - laser.point.y);

            // (옵션) 호스트 측 P 제어 결과는 디버그용으로만 사용
            IbvsOutput out = controller.update(e_u, e_v, dt_sec);

            // 디버그: 바운딩 박스 중심, 레이저 위치, 에러, 호스트 측 P제어 PWM 값을 stderr로 출력
            std::cerr << "[ctrl] frame " << frame_id
                      << " target=(" << targetROI.center().x << ", " << targetROI.center().y << ")"
                      << " laser=(" << laser.point.x << ", " << laser.point.y << ")"
                      << " e_u=" << e_u << " e_v=" << e_v
                      << " pwm(PA0,PA8)=(" << out.pan_us << ", " << out.tilt_us << ")\n";

            // Unix 파이프용: stdout에는 이제 픽셀 오차 "e_u e_v\n" 만 전송한다.
            // 신호를 너무 자주 보내지 않도록, N프레임마다 한 번씩만 전송.
            constexpr int SEND_EVERY_N_FRAMES = 3; // 3~10 프레임 사이에서 튜닝 가능
            if (frame_id % SEND_EVERY_N_FRAMES == 0)
            {
                // 예: 15.0 -10.0  (픽셀 오차, EX/EY)
                std::cout << e_u << " " << e_v << std::endl;
            }
        }

        cv::imshow("rtsp_laser_demo", frame);
        int key = cv::waitKey(1);
        if (key == 27 || key == 'q')
            break;
    }

    return 0;
}
