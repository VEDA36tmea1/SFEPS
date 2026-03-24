#include "cleanup.h"
#include <cctype>
#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>

#include "video_catalog_events.h"

namespace fs = std::filesystem;
namespace {
constexpr const char* kRecPrefix = "rec_";
constexpr const char* kMp4Suffix = ".mp4";
constexpr const char* kJpgSuffix = ".jpg";
constexpr const char* kJpegSuffix = ".jpeg";
constexpr long kCleanupIntervalSec = 10;
constexpr auto kCleanupSleepStep = std::chrono::milliseconds(200);

inline bool is_rec_mp4(const std::string& filename) {
    if (filename.rfind(kRecPrefix, 0) != 0) return false;
    return filename.size() > 4 && filename.compare(filename.size() - 4, 4, kMp4Suffix) == 0;
}

inline std::string to_lower_copy(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

inline bool is_jpeg_image(const std::string& filename) {
    const std::string lower = to_lower_copy(filename);
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, kJpgSuffix) == 0) return true;
    if (lower.size() > 5 && lower.compare(lower.size() - 5, 5, kJpegSuffix) == 0) return true;
    return false;
}

void run_image_retention_cleanup_worker(std::atomic<bool>& running_flag,
                                        const std::string& save_dir,
                                        long retention_sec,
                                        const char* log_key) {
    auto interval = std::chrono::seconds(kCleanupIntervalSec);
    while (running_flag) {
        try {
            if (fs::exists(save_dir)) {
                auto now = fs::file_time_type::clock::now();
                for (const auto& entry : fs::directory_iterator(save_dir)) {
                    if (!entry.is_regular_file()) continue;
                    const std::string filename = entry.path().filename().string();
                    if (!is_jpeg_image(filename)) continue;

                    const auto ftime = fs::last_write_time(entry);
                    const auto age =
                        std::chrono::duration_cast<std::chrono::seconds>(now - ftime).count();
                    if (age < retention_sec) continue;

                    const fs::path removed_path = entry.path();
                    if (fs::remove(removed_path)) {
                        std::cout << "[main.cpp] [" << log_key << "] removed=" << removed_path
                                  << ", age_sec=" << age
                                  << ", retention_sec=" << retention_sec << std::endl;
                    }
                }
            }
        } catch (const std::exception&) {
        }

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
} // namespace

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
                            const fs::path removed_path = entry.path();
                            if (fs::remove(removed_path)) {
                                publish_video_catalog_record_deleted_by_filename(
                                    removed_path.string());
                            }
                        }
                    }
                }
            }
        } catch (const std::exception&) {
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

void run_fraud_image_cleanup_worker(std::atomic<bool>& running_flag,
                                    const std::string& save_dir,
                                    long retention_sec) {
    run_image_retention_cleanup_worker(
        running_flag, save_dir, retention_sec, "RFID_IMAGE_RETENTION_CLEANUP");
}

void run_pending_image_cleanup_worker(std::atomic<bool>& running_flag,
                                      const std::string& save_dir,
                                      long retention_sec) {
    run_image_retention_cleanup_worker(
        running_flag, save_dir, retention_sec, "RFID_PENDING_RETENTION_CLEANUP");
}
