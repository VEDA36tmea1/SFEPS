#include <opencv2/opencv.hpp>
#include <iostream>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include "../inc/img_processing.h"

#define LIVE_CAMERA_MODE 0

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
        std::cerr << "Camera Open Error" << std::endl;
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

    std::cout << "RFID Tag Simulation (Press Enter)..." << std::endl;
    std::cin.get(); 

    int64 start_time = cv::getTickCount();

    cv::Mat target_frame;
    {
        std::lock_guard<std::mutex> lock(mtx_raw);
        if (!raw_queue.empty()) target_frame = raw_queue.front().clone();
    }

    if (target_frame.empty()) {
        is_running = false;
        cap_thread.join();
        return -1;
    }

    cv::imwrite("1_raw_FHD.jpg", target_frame);
    cv::dnn::Net net = cv::dnn::readNetFromDarknet("model/yolov4-tiny.cfg", "model/yolov4-tiny.weights");

    cv::Mat tuning_view; // 선언
    createTuningView(target_frame, tuning_view);
    cv::imwrite("2_tuning_viewer.jpg", tuning_view);
    
    is_running = false;
    cap_thread.join();
    cap.release();

#else
cv::Mat target_frame = cv::imread("img/test_image1.jpg", cv::IMREAD_COLOR);
    if (target_frame.empty()) return -1;

    int64 start_time = cv::getTickCount();

    cv::dnn::Net net = cv::dnn::readNetFromDarknet("model/yolov4-tiny.cfg", "model/yolov4-tiny.weights");

    cv::Mat tuning_view; 
    createTuningView(target_frame, tuning_view);

    cv::imwrite("3_saved_tuning_viewer_with_AI.jpg", tuning_view);
#endif

    double total_time = (cv::getTickCount() - start_time) / cv::getTickFrequency();
    std::cout << "Complete! Time: " << total_time << " sec" << std::endl;

    return 0;
}