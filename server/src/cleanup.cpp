#include "cleanup.h"
#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;
namespace {
constexpr const char* kRecPrefix = "rec_";
constexpr const char* kMp4Suffix = ".mp4";

inline bool is_rec_mp4(const std::string& filename) {
    if (filename.rfind(kRecPrefix, 0) != 0) return false;
    return filename.size() > 4 && filename.compare(filename.size() - 4, 4, kMp4Suffix) == 0;
}
} // namespace

constexpr long kCleanupIntervalSec = 10;
constexpr auto kCleanupSleepStep = std::chrono::milliseconds(200);

void run_file_cleanup_worker(std::atomic<bool>& running_flag, const std::string& save_dir, long retention_sec) {
    auto interval = std::chrono::seconds(kCleanupIntervalSec);
    while (running_flag) {
        try {
            if (fs::exists(save_dir)) {
                auto now = fs::file_time_type::clock::now();
                
                // 디렉토리 반복 (Iterator)
                for (const auto& entry : fs::directory_iterator(save_dir)) {
                    if (entry.is_regular_file()) {
                        std::string filename = entry.path().filename().string();

                        if (!is_rec_mp4(filename)) continue;
                            
                        // 생성 시간 체크
                        auto ftime = fs::last_write_time(entry);
                        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - ftime).count();
                        
                        if (age >= retention_sec) {
                            //std::cout << "[Cleanup] Del: " << filename << " (Age: " << age << "s)" << std::endl;
                            fs::remove(entry.path());
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[Cleanup Error] " << e.what() << std::endl;
        }
        
        // 종료 신호가 오면 10초를 다 기다리지 않고 빠르게 종료한다.
        auto waited = std::chrono::milliseconds(0);
        while (running_flag && waited < interval) {
            const auto remain =
                std::chrono::duration_cast<std::chrono::milliseconds>(interval - waited);
            const auto chunk = (remain < kCleanupSleepStep) ? remain : kCleanupSleepStep;
            std::this_thread::sleep_for(chunk);
            waited += chunk;
        }
    }
}
