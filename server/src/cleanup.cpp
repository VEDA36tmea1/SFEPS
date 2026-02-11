#include "cleanup.h"
#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

void run_file_cleanup_worker(std::atomic<bool>& running_flag, const std::string& save_dir, long retention_sec) {
    while (running_flag) {
        try {
            if (fs::exists(save_dir)) {
                auto now = fs::file_time_type::clock::now();
                
                // 디렉토리 반복 (Iterator)
                for (const auto& entry : fs::directory_iterator(save_dir)) {
                    if (entry.is_regular_file()) {
                        std::string filename = entry.path().filename().string();

                        // "rec_"로 시작하고 ".mp4"로 끝나는 파일만 대상
                        if (filename.rfind("rec_", 0) == 0 && filename.length() >= 4 && 
                            filename.compare(filename.length() - 4, 4, ".mp4") == 0) {
                            
                            // 생성 시간 체크
                            auto ftime = fs::last_write_time(entry);
                            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - ftime).count();
                            
                            if (age >= retention_sec) {
                                std::cout << "[Cleanup] Del: " << filename << " (Age: " << age << "s)" << std::endl;
                                fs::remove(entry.path());
                            }
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[Cleanup Error] " << e.what() << std::endl;
        }
        
        // 10초마다 검사
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}