#include <opencv2/opencv.hpp>
#include <iostream>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable> 
#include <fcntl.h>             
#include <unistd.h>         
#include <csignal>
#include "../inc/img_processing.h"

// 0이면 로컬 이미지 테스트, 1이면 실제 카메라 가동
#define LIVE_CAMERA_MODE 1

std::atomic<bool> is_running{true}; 
std::mutex mtx_raw;
std::queue<cv::Mat> raw_queue;

std::condition_variable cv_rfid;
std::mutex mtx_rfid;
bool rfid_detected = false;

// 종료 시그널 핸들러
void signalHandler(int signum) {
    std::cout << "\n[종료] 시스템을 정리합니다." << std::endl;
    is_running = false;
    cv_rfid.notify_all(); 
}

#if LIVE_CAMERA_MODE
// 카메라 캡처 스레드: 항상 최신 프레임을 큐에 유지
void captureThreadFunc(cv::VideoCapture& cap) {
    while (is_running) {
        cv::Mat tmp;
        if (cap.read(tmp)) {
            std::lock_guard<std::mutex> lock(mtx_raw);
            if (!raw_queue.empty()) raw_queue.pop(); // 최신 프레임만 유지 (drop=true 효과)
            raw_queue.push(tmp);
        }
    }
}

// RFID 감지 스레드
void rfidThreadFunc() {
    int fd = open("/dev/rc522", O_RDONLY);
    if (fd < 0) {
        std::cerr << "❌ RFID 열기 실패 (sudo 권한 확인)" << std::endl;
        return;
    }
    uint8_t uid[4];
    while (is_running) {
        if (read(fd, uid, sizeof(uid)) == 4) {
            std::cout << "💳 RFID 태그됨! UID: " << std::hex << (int)uid[0] << ":" << (int)uid[1] << std::dec << std::endl;
            {
                std::lock_guard<std::mutex> lock(mtx_rfid);
                rfid_detected = true;
            }
            cv_rfid.notify_one(); 
            std::this_thread::sleep_for(std::chrono::seconds(2)); // 중복 방지
        }
    }
    close(fd);
}
#endif

int main() {
    signal(SIGINT, signalHandler);

#if LIVE_CAMERA_MODE
    // 파이프라인: libcamerasrc 사용 (Pi OS 최신 권장 방식)
    std::string in_pipe = "libcamerasrc ! video/x-raw, width=1920, height=1080, framerate=30/1 ! videoconvert ! video/x-raw, format=BGR ! appsink drop=true max-buffers=1";
    cv::VideoCapture cap(in_pipe, cv::CAP_GSTREAMER);

    if (!cap.isOpened()) {
        std::cerr << "❌ 카메라 파이프라인 열기 실패" << std::endl;
        return -1;
    }

    std::thread cap_thread(captureThreadFunc, std::ref(cap));
    std::thread rfid_thread(rfidThreadFunc);

    while (is_running) {
        std::cout << "⌛ 태그 대기 중..." << std::endl;
        std::unique_lock<std::mutex> lock(mtx_rfid);
        cv_rfid.wait(lock, []{ return rfid_detected || !is_running; });
        if (!is_running) break;
        rfid_detected = false; 

        // 큐에서 최신 프레임 꺼내기
        cv::Mat target_frame;
        {
            std::lock_guard<std::mutex> lock_raw(mtx_raw);
            if (!raw_queue.empty()) target_frame = raw_queue.front().clone();
        }

        if (target_frame.empty()) continue;

        std::cout << "📸 찰칵! 사진 처리 시작..." << std::endl;
        cv::imwrite("1_raw_capture.jpg", target_frame);

        cv::Mat tuning_view; 
        cv::Mat best_frame = processISPAndGetBest(target_frame, tuning_view);
        
        cv::imwrite("2_tuning_viewer.jpg", tuning_view);
        cv::imwrite("3_best_shot.jpg", best_frame);
        std::cout << "✅ 저장 완료!" << std::endl;
    }

    if (cap_thread.joinable()) cap_thread.join();
    if (rfid_thread.joinable()) rfid_thread.join();
    cap.release();

#else
    // 로컬 테스트 모드
    cv::Mat target_frame = cv::imread("img/test_image4.jpg", cv::IMREAD_COLOR);
    if (target_frame.empty()) {
        std::cerr << "❌ 이미지 로드 실패" << std::endl;
        return -1;
    }

    cv::Mat tuning_view; 
    cv::Mat best_frame = processISPAndGetBest(target_frame, tuning_view);

    cv::imwrite("4_tuning_viewer_local.jpg", tuning_view);
    cv::imwrite("5_best_shot_local.jpg", best_frame);
    std::cout << "✅ 로컬 테스트 완료" << std::endl;
#endif

    std::cout << "\n✅ 시스템이 안전하게 종료되었습니다." << std::endl;
    return 0;
}