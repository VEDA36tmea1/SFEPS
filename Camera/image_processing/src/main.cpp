#include <opencv2/opencv.hpp>
#include <iostream>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include "../inc/img_processing.h"


// 1 : 라이브 카메라 촬영 모드 (RFID 태그 대기 -> 촬영 -> 8분할 뷰어)
// 0 : 저장된 사진 테스트 모드 (하드디스크의 이미지 읽기 -> 8분할 뷰어)
#define LIVE_CAMERA_MODE 1

#if LIVE_CAMERA_MODE
std::mutex mtx_raw;
std::queue<cv::Mat> raw_queue;
std::atomic<bool> is_running{true}; 

void captureThreadFunc(cv::VideoCapture& cap) {
    while (is_running) {
        cv::Mat tmp;
        if (cap.read(tmp)) {
            cv::Mat safe_copy = tmp.clone(); 
            std::lock_guard<std::mutex> lock(mtx_raw);
            if (raw_queue.size() > 1) raw_queue.pop(); 
            raw_queue.push(safe_copy);
        }
    }
}
#endif

int main() {
#if LIVE_CAMERA_MODE
    std::string in_pipe = "libcamerasrc ! video/x-raw, width=1920, height=1080, framerate=30/1 ! videoconvert ! video/x-raw, format=BGR ! appsink drop=true max-buffers=1";
    cv::VideoCapture cap(in_pipe, cv::CAP_GSTREAMER);

    if (!cap.isOpened()) {
        std::cerr << " Error : 카메라를 열 수 없습니다." << std::endl;
        return -1;
    }

    std::thread cap_thread(captureThreadFunc, std::ref(cap));

    cv::Mat frame;
    while (is_running) {
        std::lock_guard<std::mutex> lock(mtx_raw);
        if (!raw_queue.empty()) {
            frame = raw_queue.front().clone();
            break;
        }
    }

    std::cout << "\n==========================================" << std::endl;
    std::cout << " RFID 태그를 찍으세요 (Enter 입력)..." << std::endl;
    std::cout << "==========================================" << std::endl;
    std::cin.get(); 

    int64 start_time = cv::getTickCount();

    cv::Mat target_frame;
    {
        std::lock_guard<std::mutex> lock(mtx_raw);
        if (!raw_queue.empty()) {
            target_frame = raw_queue.front().clone();
        }
    }

    if (target_frame.empty()) {
        std::cerr << "사진 로딩 실패" << std::endl;
        is_running = false;
        cap_thread.join();
        return -1;
    }

    cv::imwrite("1_raw_FHD.jpg", target_frame);
    cv::Mat tuning_view;
    createTuningView(target_frame, tuning_view);

    cv::imwrite("2_tuning_viewer.jpg", tuning_view);
    
    double total_time = (cv::getTickCount() - start_time) / cv::getTickFrequency();
    std::cout << "8분할 사진 저장 완료 (소요 시간: " << total_time << " 초)" << std::endl;

    is_running = false;
    cap_thread.join();
    cap.release();

#else
    std::string input_filename = "img/test_image1.jpg"; 
 
    cv::Mat target_frame = cv::imread(input_filename, cv::IMREAD_COLOR);

    if (target_frame.empty()) {
        std::cerr << "Error : 사진을 찾을 수 없거나 읽을 수 없습니다 (" << input_filename << " 파일 확인)" << std::endl;
        return -1;
    }

    int64 start_time = cv::getTickCount();

    createTuningView(target_frame, tuning_view);

    std::string output_filename = "3_saved_tuning_viewer.jpg";
    cv::imwrite(output_filename, tuning_view);
    
    double total_time = (cv::getTickCount() - start_time) / cv::getTickFrequency();
    std::cout << "8분할 비교 사진 저장 완료 (파일: " << output_filename << ", 소요 시간: " << total_time << " 초)" << std::endl;

#endif

    return 0;
}