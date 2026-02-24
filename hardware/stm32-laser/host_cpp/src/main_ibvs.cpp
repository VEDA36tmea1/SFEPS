#include <chrono>
#include <iostream>

#include <opencv2/highgui.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/imgproc.hpp>

#include "vision_detector.h"
#include "ibvs_controller.h"
#include "stm_interface.h"

int main(int argc, char** argv)
{
    std::string serial_dev = "/dev/ttyUSB0";
    int baudrate = 115200;
    int camera_index = 0;

    if (argc >= 2)
        serial_dev = argv[1];
    if (argc >= 3)
        camera_index = std::atoi(argv[2]);

    std::cout << "Using serial=" << serial_dev
              << ", baudrate=" << baudrate
              << ", camera_index=" << camera_index << std::endl;

    StmInterface stm(serial_dev, baudrate);
    if (!stm.isOpen())
    {
        std::cerr << "Failed to open STM serial port. Continue without sending commands.\n";
    }
    else
    {
        // IBVS 운용 전 manual 모드로 강제 전환
        stm.sendModeManual();
    }

    cv::VideoCapture cap(camera_index);
    if (!cap.isOpened())
    {
        std::cerr << "Failed to open camera index " << camera_index << std::endl;
        return 1;
    }

    VisionDetector detector;

    constexpr int PWM_MIN = 800;
    constexpr int PWM_MAX = 2200;
    constexpr int PWM_NEUTRAL = 1500;

    // 초기 예제 게인 (필드에서 반드시 조정 필요)
    double Ku = 0.5; // [us / pixel]
    double Kv = 0.5; // [us / pixel]

    IbvsController controller(PWM_MIN, PWM_MAX, PWM_NEUTRAL, Ku, Kv);

    auto last_time = std::chrono::steady_clock::now();

    while (true)
    {
        cv::Mat frame;
        if (!cap.read(frame) || frame.empty())
        {
            std::cerr << "Failed to read frame from camera\n";
            break;
        }

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_time).count();
        last_time = now;

        DetectionResult target = detector.detectTarget(frame);
        DetectionResult laser  = detector.detectLaser(frame);

        if (target.found && laser.found)
        {
            double e_u = static_cast<double>(target.point.x - laser.point.x);
            double e_v = static_cast<double>(target.point.y - laser.point.y);

            IbvsOutput out = controller.update(e_u, e_v, dt);

            if (stm.isOpen())
            {
                stm.sendPwm(out.pan_us, out.tilt_us);
            }

            // 디버그용 overlay
            cv::circle(frame, target.point, 5, cv::Scalar(0, 255, 0), 2); // 타겟: 초록
            cv::circle(frame, laser.point,  5, cv::Scalar(0, 0, 255), 2); // 레이저: 빨강
            cv::line(frame, target.point, laser.point, cv::Scalar(255, 0, 0), 1);

            std::cout << "e_u=" << e_u << ", e_v=" << e_v
                      << "  pan=" << out.pan_us << ", tilt=" << out.tilt_us << "\n";
        }
        else
        {
            // 검출 실패 시에는 단순히 overlay만 표시 (실제 시스템에서는 안전 로직 추가 필요)
            std::cout << "Detection failed: target_found=" << target.found
                      << ", laser_found=" << laser.found << "\n";
        }

        cv::imshow("IBVS Laser View", frame);
        int key = cv::waitKey(1);
        if (key == 27 || key == 'q') // ESC 또는 q
            break;
    }

    return 0;
}

