#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <opencv2/opencv.hpp>
#include "../inc/img_processing.h"

std::atomic<bool> is_running{true};
std::mutex mtx_rfid;
std::condition_variable cv_rfid;
bool rfid_detected = false;

// 종료 시그널 핸들러
void signalHandler(int signum) {
    std::cout << "\n[종료] 시스템을 정리합니다." << std::endl;
    is_running = false;
    cv_rfid.notify_all();
}

// RFID 비동기 감지 스레드
void rfidThreadFunc() {
    int fd = open("/dev/rc522", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "❌ RFID 장치 열기 실패" << std::endl;
        return;
    }
    uint8_t uid[4];
    while (is_running) {
        ssize_t bytes_read = read(fd, uid, sizeof(uid));
        if (bytes_read == 4) {
            std::cout << "\n💳 RFID 감지됨!" << std::endl;
            {
                std::lock_guard<std::mutex> lock(mtx_rfid);
                rfid_detected = true;
            }
            cv_rfid.notify_one();
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    close(fd);
}

int main() {
    signal(SIGINT, signalHandler);
    std::thread rfid_thread(rfidThreadFunc);

    int width = 3280, height = 2464;
    int fd = open("/dev/video0", O_RDWR);

    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_SBGGR10;
    ioctl(fd, VIDIOC_S_FMT, &fmt);

    struct v4l2_requestbuffers req = {1, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP};
    ioctl(fd, VIDIOC_REQBUFS, &req);

    struct v4l2_buffer buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    ioctl(fd, VIDIOC_QUERYBUF, &buf);

    void* buffer_start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);

    int type = buf.type;
    ioctl(fd, VIDIOC_STREAMON, &type);

    while (is_running) {
        std::cout << "⌛ 태그를 기다리는 중..." << std::endl;
        std::unique_lock<std::mutex> lock(mtx_rfid);
        cv_rfid.wait(lock, [] { return rfid_detected || !is_running; });
        if (!is_running) break;
        rfid_detected = false;

        // V4L2로 센서에서 진짜 RAW 데이터 한 장 가져오기
        ioctl(fd, VIDIOC_QBUF, &buf);
        ioctl(fd, VIDIOC_DQBUF, &buf);

        std::cout << "📸 캡처 완료. 순수 C++ ISP 엔진 가동 중..." << std::endl;

        // ========================================================
        // 🌟 여기가 핵심입니다! 엔진에 데이터를 밀어 넣는 과정
        // ========================================================
        
        // 1. 센서 메모리 포인터(buffer_start)를 OpenCV가 다루기 쉽게 포장만 해줍니다.
        cv::Mat raw16_mat(height, width, CV_16UC1, buffer_start);

        // 2. 아까 업그레이드한 100% 순수 C++ ISP 엔진(BLC -> AWB -> Demosaic) 통과!
        cv::Mat bgr_frame = runPureISP(raw16_mat);
        cv::imwrite("1_isp_processed.jpg", bgr_frame);
        std::cout << "✅ 1차 가공 완료 (1_isp_processed.jpg)" << std::endl;

        // 3. 기존 앱 계층의 화질 튜닝(ShadowBoost/CLAHE) 및 베스트샷 선정
        cv::Mat tuning_view;
        cv::Mat best_frame = processISPAndGetBest(bgr_frame, tuning_view);

        cv::imwrite("2_tuning_viewer.jpg", tuning_view);
        cv::imwrite("3_best_shot.jpg", best_frame);
        std::cout << "✅ 화질 튜닝 및 베스트샷 저장 완료!" << std::endl;
        std::cout << "------------------------------------------\n" << std::endl;
    }

    if (rfid_thread.joinable()) {
        rfid_thread.detach();
    }

    ioctl(fd, VIDIOC_STREAMOFF, &type);
    munmap(buffer_start, buf.length);
    close(fd);

    std::cout << "\n✅ 모든 시스템이 안전하게 종료되었습니다." << std::endl;
    return 0;
}