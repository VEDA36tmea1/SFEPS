#include <opencv2/opencv.hpp>
#include <iostream>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable> 
#include <fcntl.h>            
#include <unistd.h>        
#include "../inc/img_processing.h"

#define LIVE_CAMERA_MODE 0

#if LIVE_CAMERA_MODE
std::mutex mtx_raw;
std::queue<cv::Mat> raw_queue;
std::atomic<bool> is_running{true}; 
// RFID 신호 전달을 위한 전역 변수
std::condition_variable cv_rfid;
std::mutex mtx_rfid;
bool rfid_detected = false;

// 카메라 캡처 스레드
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

// RFID 하드웨어 감지 전담 스레드
void rfidThreadFunc() {
    int fd = open("/dev/rc522", O_RDONLY);
    if (fd < 0) {
        std::cerr << "RFID 열기 실패" << std::endl;
        return;
    }

    uint8_t uid[4];
    while (is_running) {
        ssize_t bytes_read = read(fd, uid, sizeof(uid));
        
        if (bytes_read == 4) {
            std::cout << "\n[RFID] 카드 인식 UID: " 
                      << std::hex << (int)uid[0] << ":" << (int)uid[1] << ":" 
                      << (int)uid[2] << ":" << (int)uid[3] << std::dec << std::endl;

            // 메인 스레드 깨우기
            {
                std::lock_guard<std::mutex> lock(mtx_rfid);
                rfid_detected = true;
            }
            cv_rfid.notify_one(); 

            // 카드를 대고 있을 때 중복 촬영되는 것 방지 (2초 휴식)
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
    close(fd);
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
    std::thread rfid_thread(rfidThreadFunc); // RFID 스레드 가동

    // 첫 프레임 대기
    cv::Mat frame;
    while (is_running) {
        std::lock_guard<std::mutex> lock(mtx_raw);
        if (!raw_queue.empty()) {
            frame = raw_queue.front().clone();
            break;
        }
    }

    std::cout << "\nRFID 카드를 태그해 주세요" << std::endl;
    {
        // RFID 인터럽트 대기
        std::unique_lock<std::mutex> lock(mtx_rfid);
        // rfid_detected가 true가 될 때까지 여기서 멈춰서 기다림
        cv_rfid.wait(lock, []{ return rfid_detected || !is_running; });
        rfid_detected = false; // 다음을 위해 초기화
    }
    // ==========================================================

    int64 start_time = cv::getTickCount(); // 전체 처리시간
    int64 capture_start = cv::getTickCount(); // 캡처 시간

    cv::Mat target_frame;
    {
        std::lock_guard<std::mutex> lock(mtx_raw);
        if (!raw_queue.empty()) target_frame = raw_queue.front().clone();
    }

    double capture_time = (cv::getTickCount() - capture_start) / cv::getTickFrequency();
    std::cout << "소요 시간: " << capture_time << " 초" << std::endl;

    if (target_frame.empty()) {
        is_running = false;
        cap_thread.join();
        rfid_thread.join(); // 추가
        return -1;
    }

    cv::imwrite("1_raw_FHD.jpg", target_frame);

    cv::Mat tuning_view; 
    createTuningView(target_frame, tuning_view);
    cv::imwrite("2_tuning_viewer.jpg", tuning_view);
    
    is_running = false;
    cap_thread.join();
    rfid_thread.join(); // 추가
    cap.release();

#else
    cv::Mat target_frame = cv::imread("img/test_image1.jpg", cv::IMREAD_COLOR);
    if (target_frame.empty()) return -1;

    int64 start_time = cv::getTickCount();
    cv::Mat tuning_view; 
    createTuningView(target_frame, tuning_view);

    cv::imwrite("3_saved_tuning_viewer_with_AI.jpg", tuning_view);
#endif

    double total_time = (cv::getTickCount() - start_time) / cv::getTickFrequency();
    std::cout << "Complete! Time: " << total_time << " sec" << std::endl;

    return 0;
}