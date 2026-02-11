#include <iostream>
#include <filesystem>
#include <thread>
#include <atomic>
#include "log.h"
#include "recorder.h" 
#include "cleanup.h" // 분리된 cleanup 헤더 추가

namespace fs = std::filesystem;

int main() {
    // 1. 디렉토리 생성 (recorder.h에 정의된 상수 사용)
    if (!fs::exists(VIDEO_SAVE_DIR)) fs::create_directories(VIDEO_SAVE_DIR);

    // 2. DB 연결
    DBLogger logger;
    if (!logger.connect()) {
        std::cerr << "[Fatal] DB Connection failed." << std::endl;
        return -1;
    }

    // 3. 실행 플래그
    std::atomic<bool> running(true);

    // 4. 파일 정리 스레드 시작 (분리된 함수 호출)
    // VIDEO_SAVE_DIR 경로를 인자로 넘겨줍니다.
    std::thread t1(run_file_cleanup_worker, std::ref(running), std::string(VIDEO_SAVE_DIR), 300);
    t1.detach();

    // 5. DB 정리 스레드 시작
    std::thread t2([&](){ 
        while(running) { 
            std::this_thread::sleep_for(std::chrono::seconds(60)); 
            logger.requestDbCleanup(); 
        } 
    });
    t2.detach();

    // 6. 녹화 시작
    RTSPRecorder recorder(logger, running);
    recorder.run(); // 메인 스레드 블로킹

    running = false;
    std::cout << "[System] Server shutdown." << std::endl;
    return 0;
}