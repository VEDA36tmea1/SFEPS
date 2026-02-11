#include <iostream>
#include <filesystem>
#include <thread>
#include <atomic>
#include <csignal> // 시그널 처리를 위해 필요
#include "log.h"
#include "recorder.h" 
#include "cleanup.h" 

namespace fs = std::filesystem;

// 전역 플래그 (시그널 핸들러에서 접근하기 위해)
std::atomic<bool> g_running(true);

// [핵심] Ctrl+C 감지 함수
void signal_handler(int signum) {
    std::cout << "\n[System] 종료 신호 감지! 녹화를 저장하고 종료합니다...\n";
    g_running = false; // 루프를 멈추게 함 -> 자연스럽게 저장 로직 실행됨
}

int main() {
    // 1. 종료 신호(SIGINT) 등록
    signal(SIGINT, signal_handler);

    // 2. 디렉토리 생성
    if (!fs::exists(VIDEO_SAVE_DIR)) fs::create_directories(VIDEO_SAVE_DIR);

    // 3. DB 연결
    DBLogger logger;
    if (!logger.connect()) {
        std::cerr << "[Fatal] DB Connection failed." << std::endl;
        return -1;
    }

    // 4. 파일 정리 스레드 시작
    // 전역 변수 g_running을 참조로 넘김
    std::thread t1(run_file_cleanup_worker, std::ref(g_running), std::string(VIDEO_SAVE_DIR), 300);
    t1.detach();

    // 5. DB 정리 스레드 시작
    std::thread t2([&](){ 
        while(g_running) { 
            std::this_thread::sleep_for(std::chrono::seconds(60)); 
            logger.requestDbCleanup(); 
        } 
    });
    t2.detach();

    // 6. 녹화 시작
    // 여기서 g_running이 false가 되면 recorder.run() 내부의 루프가 끝나고
    // 자동으로 close_current_file()이 호출되어 파일이 정상 저장됩니다.
    RTSPRecorder recorder(logger, g_running);
    recorder.run();

    std::cout << "[System] 서버가 안전하게 종료되었습니다." << std::endl;
    return 0;
}