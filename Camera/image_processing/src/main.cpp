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

#define LIVE_CAMERA_MODE 0

std::atomic<bool> is_running{true}; 
std::mutex mtx_raw;
std::queue<cv::Mat> raw_queue;
std::condition_variable cv_rfid;
std::mutex mtx_rfid;
bool rfid_detected = false;

void signalHandler(int signum) {
    std::cout << "\n\n[인터럽트 감지] 시스템을 종료합니다." << std::endl;
    is_running = false;
    cv_rfid.notify_all(); // 메인 스레드 호출
}

#if LIVE_CAMERA_MODE
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

void rfidThreadFunc() {
    int fd = open("/dev/rc522", O_RDONLY);
    if (fd < 0) {
        std::cerr << "❌ RFID 열기 실패 (sudo를 붙여 실행해주세요.)" << std::endl;
        return;
    }
    uint8_t uid[4];
    while (is_running) {
        ssize_t bytes_read = read(fd, uid, sizeof(uid));
        if (bytes_read == 4) {
            std::cout << "\nRFID 카드가 태그되었습니다."; 
            std::cout << "\nUID:"  << std::hex
                      << (int)uid[0] << ":" << (int)uid[1] << ":" 
                      << (int)uid[2] << ":" << (int)uid[3] << std::dec << std::endl;
            {
                std::lock_guard<std::mutex> lock(mtx_rfid);
                rfid_detected = true;
            }
            cv_rfid.notify_one(); 
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
    close(fd);
}
#endif

int main() {
    // 시그널 핸들러 등록
    signal(SIGINT, signalHandler);

#if LIVE_CAMERA_MODE
    std::string in_pipe = "libcamerasrc ! video/x-raw, width=1920, height=1080, framerate=30/1 ! videoconvert ! video/x-raw, format=BGR ! appsink drop=true max-buffers=1";
    cv::VideoCapture cap(in_pipe, cv::CAP_GSTREAMER);

    if (!cap.isOpened()) {
        std::cerr << "Camera Open Error" << std::endl;
        return -1;
    }

    std::thread cap_thread(captureThreadFunc, std::ref(cap));
    std::thread rfid_thread(rfidThreadFunc); 

    cv::Mat frame;
    while (is_running) {
        std::lock_guard<std::mutex> lock(mtx_raw);
        if (!raw_queue.empty()) {
            frame = raw_queue.front().clone();
            break;
        }
    }

    while (is_running) {
        std::cout << "\n==========================================" << std::endl;
        std::cout << "태그 대기 중입니다. (종료하려면 Ctrl+C를 눌러주세요.)" << std::endl;
        std::cout << "==========================================\n" << std::endl;

        {
            std::unique_lock<std::mutex> lock(mtx_rfid);
            cv_rfid.wait(lock, []{ return rfid_detected || !is_running; });
            if (!is_running) break;
            rfid_detected = false; 
        }

        int64 capture_start = cv::getTickCount(); 
        cv::Mat target_frame;
        {
            std::lock_guard<std::mutex> lock(mtx_raw);
            if (!raw_queue.empty()) target_frame = raw_queue.front().clone();
        }

        if (target_frame.empty()) continue;

        double capture_time = (cv::getTickCount() - capture_start) / cv::getTickFrequency();
        std::cout << "사진 촬영 완료 (" << capture_time << "초)" << std::endl;

        cv::imwrite("1_raw_image.jpg", target_frame);
        
        cv::Mat tuning_view; 
        cv::Mat best_frame = processISPAndGetBest(target_frame, tuning_view);
        
        cv::imwrite("2_tuning_viewer.jpg", tuning_view);
        cv::imwrite("3_best_shot.jpg", best_frame);
       
    }

    // 종료 절차
    cap_thread.join();
    rfid_thread.join(); 
    cap.release();

#else
    // 로컬 이미지 테스트 모드
    cv::Mat target_frame = cv::imread("img/test_image4.jpg", cv::IMREAD_COLOR);
    if (target_frame.empty()) {
        std::cerr << "❌ 이미지 로드 실패" << std::endl;
        return -1;
    }

    int64 start_time = cv::getTickCount();
    
    cv::Mat tuning_view; 
    cv::Mat best_frame = processISPAndGetBest(target_frame, tuning_view);

    cv::imwrite("4_tuning_viewer_local.jpg", tuning_view);
    cv::imwrite("5_best_shot_local.jpg", best_frame);

    double total_time = (cv::getTickCount() - start_time) / cv::getTickFrequency();
    std::cout << "소요시간 : " << total_time << " 초" << std::endl;
#endif

    std::cout << "\n시스템이 종료되었습니다." << std::endl;
    return 0;
}