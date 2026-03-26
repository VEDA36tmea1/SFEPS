#include "cleanup.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

#include "video_catalog_events.h"

namespace fs = std::filesystem;
namespace {
constexpr const char* kRecPrefix = "rec_";
constexpr const char* kMp4Suffix = ".mp4";
constexpr const char* kJpgSuffix = ".jpg";
constexpr const char* kJpegSuffix = ".jpeg";
constexpr long kCleanupIntervalSec = 10;
constexpr auto kCleanupSleepStep = std::chrono::milliseconds(200);

struct VideoFileEntry {
    fs::path path;
    fs::file_time_type last_write_time;
    std::uintmax_t size_bytes = 0;
};

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

std::uintmax_t collect_video_files(const std::string& save_dir, std::vector<VideoFileEntry>& out_files) {
    out_files.clear();
    std::uintmax_t total_bytes = 0;

    for (const auto& entry : fs::directory_iterator(save_dir)) {
        if (!entry.is_regular_file()) continue;
        const std::string filename = entry.path().filename().string();
        if (!is_rec_mp4(filename)) continue;

        std::error_code ec;
        const auto size = entry.file_size(ec);
        if (ec) continue;

        out_files.push_back(VideoFileEntry{entry.path(), fs::last_write_time(entry), size});
        total_bytes += size;
    }

    std::sort(out_files.begin(), out_files.end(),
              [](const VideoFileEntry& lhs, const VideoFileEntry& rhs) {
                  return lhs.last_write_time < rhs.last_write_time;
              });
    return total_bytes;
}

bool remove_video_file(const VideoFileEntry& file,
                       const char* reason,
                       long age_sec,
                       long retention_sec,
                       std::uintmax_t current_total_bytes,
                       std::uintmax_t max_storage_bytes,
                       std::uintmax_t& total_bytes,
                       std::size_t& deleted_count) {
    std::error_code ec;
    const bool removed = fs::remove(file.path, ec);
    if (!removed || ec) return false;

    publish_video_catalog_record_deleted_by_filename(file.path.string());
    if (total_bytes >= file.size_bytes) {
        total_bytes -= file.size_bytes;
    } else {
        total_bytes = 0;
    }
    ++deleted_count;
    (void)reason;
    (void)age_sec;
    (void)retention_sec;
    (void)current_total_bytes;
    (void)max_storage_bytes;
    return true;
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
    std::cout << "[main.cpp] [" << log_key << "] 종료." << std::endl;
}
} // namespace

void run_file_cleanup_worker(std::atomic<bool>& running_flag,
                             const std::string& save_dir,
                             long retention_sec,
                             std::uintmax_t max_storage_bytes) {
    auto interval = std::chrono::seconds(kCleanupIntervalSec);
    while (running_flag) {
        try {
            if (fs::exists(save_dir)) {
                auto now = fs::file_time_type::clock::now();
                std::vector<VideoFileEntry> files;
                std::uintmax_t total_bytes = collect_video_files(save_dir, files);
                const std::uintmax_t initial_total_bytes = total_bytes;
                std::size_t deleted_count = 0;

                for (const auto& file : files) {
                    const auto age =
                        std::chrono::duration_cast<std::chrono::seconds>(now - file.last_write_time)
                            .count();
                    if (age < retention_sec) continue;

                    const auto total_before_delete = total_bytes;
                    remove_video_file(file, "retention", age, retention_sec, total_before_delete,
                                      max_storage_bytes, total_bytes, deleted_count);
                }

                if (max_storage_bytes > 0 && total_bytes > max_storage_bytes) {
                    for (const auto& file : files) {
                        if (total_bytes <= max_storage_bytes) break;
                        if (!fs::exists(file.path)) continue;

                        const auto total_before_delete = total_bytes;
                        remove_video_file(file, "storage_limit", -1, retention_sec,
                                          total_before_delete, max_storage_bytes, total_bytes,
                                          deleted_count);
                    }
                }

                (void)initial_total_bytes;
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
    std::cout << "[main.cpp] [VIDEO_RETENTION_CLEANUP] 종료." << std::endl;
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
