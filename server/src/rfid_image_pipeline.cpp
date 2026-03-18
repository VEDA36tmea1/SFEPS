#include "rfid_image_pipeline.h"

#include <chrono>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#include "recorder.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kRfidImageSourcePath =
    "/home/iam/SFEPS/Camera/image_processing/3_best_shot.jpg";
constexpr const char* kEventImageBaseDir = "/home/iam/SFEPS/event_images";
constexpr const char* kEventImagePendingDir = "/home/iam/SFEPS/event_images/pending";
constexpr const char* kEventImageFraudDir = "/home/iam/SFEPS/event_images/fraud";
constexpr const char* kEventImageFailedDir = "/home/iam/SFEPS/event_images/failed";

struct EventImageRegistry {
    std::mutex mutex;
    std::unordered_map<std::string, fs::path> pending_by_object_id;
};

EventImageRegistry& event_image_registry() {
    static EventImageRegistry registry;
    return registry;
}

std::uint64_t unix_epoch_ms_now() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string sanitize_filename_token(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (std::isalnum(c) != 0 || c == '-' || c == '_') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "unknown" : out;
}

fs::path make_unique_path(const fs::path& preferred) {
    if (!fs::exists(preferred)) return preferred;

    const fs::path dir = preferred.parent_path();
    const std::string stem = preferred.stem().string();
    const std::string ext = preferred.extension().string();
    for (int i = 0; i < 1000; ++i) {
        fs::path candidate = dir / (stem + "_" + std::to_string(unix_epoch_ms_now()) + "_" +
                                    std::to_string(i) + ext);
        if (!fs::exists(candidate)) return candidate;
    }

    return dir / (stem + "_" + std::to_string(unix_epoch_ms_now()) + "_overflow" + ext);
}

fs::path move_file_to_dir(const fs::path& source, const fs::path& target_dir) {
    const fs::path target = make_unique_path(target_dir / source.filename());
    std::error_code ec;
    fs::rename(source, target, ec);
    if (!ec) return target;

    ec.clear();
    if (fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec)) {
        std::error_code remove_ec;
        fs::remove(source, remove_ec);
        return target;
    }

    throw std::runtime_error("failed to move image file: " + source.string() + " -> " +
                             target.string() + " (" + ec.message() + ")");
}

}  // namespace

bool ensure_runtime_media_dirs(std::string& err) {
    try {
        if (!fs::exists(VIDEO_SAVE_DIR)) {
            fs::create_directories(VIDEO_SAVE_DIR);
        }
        fs::create_directories(kEventImageBaseDir);
        fs::create_directories(kEventImagePendingDir);
        fs::create_directories(kEventImageFraudDir);
        fs::create_directories(kEventImageFailedDir);
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

void snapshot_rfid_image_for_object(const std::string& object_id) {
    if (object_id.empty()) return;

    auto& registry = event_image_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    try {
        const fs::path source(kRfidImageSourcePath);
        if (!fs::exists(source) || !fs::is_regular_file(source)) {
            std::cout << "[main.cpp] [RFID_IMAGE_SNAP] source image missing: " << source
                      << ", object_id=" << object_id << std::endl;
            return;
        }

        const auto existing = registry.pending_by_object_id.find(object_id);
        if (existing != registry.pending_by_object_id.end()) {
            std::error_code remove_ec;
            fs::remove(existing->second, remove_ec);
        }

        const fs::path target = make_unique_path(
            fs::path(kEventImagePendingDir) /
            ("rfid_" + std::to_string(unix_epoch_ms_now()) + "_" +
             sanitize_filename_token(object_id) + ".jpg"));

        fs::copy_file(source, target, fs::copy_options::overwrite_existing);
        registry.pending_by_object_id[object_id] = target;
        std::cout << "[main.cpp] [RFID_IMAGE_SNAP] object_id=" << object_id
                  << ", source=" << source << ", saved=" << target << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[main.cpp] [RFID_IMAGE_SNAP] failed: object_id=" << object_id
                  << ", err=" << e.what() << std::endl;
    }
}

void finalize_outline_image_for_object(
    const AnalyticsProcessor::OutlineDecisionPayload& payload) {
    if (payload.object_id.empty()) return;

    auto& registry = event_image_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);

    const auto it = registry.pending_by_object_id.find(payload.object_id);
    if (it == registry.pending_by_object_id.end()) {
        std::cout << "[main.cpp] ["
                  << (payload.is_fraud ? "RFID_IMAGE_KEEP" : "RFID_IMAGE_DELETE")
                  << "] no pending image: object_id=" << payload.object_id
                  << ", tag_time=" << payload.tag_time << std::endl;
        return;
    }

    const fs::path pending_path = it->second;
    registry.pending_by_object_id.erase(it);

    if (!fs::exists(pending_path)) {
        std::cout << "[main.cpp] ["
                  << (payload.is_fraud ? "RFID_IMAGE_KEEP" : "RFID_IMAGE_DELETE")
                  << "] pending image missing on disk: object_id=" << payload.object_id
                  << ", path=" << pending_path << ", tag_time=" << payload.tag_time
                  << std::endl;
        return;
    }

    if (!payload.is_fraud) {
        std::error_code remove_ec;
        if (fs::remove(pending_path, remove_ec)) {
            std::cout << "[main.cpp] [RFID_IMAGE_DELETE] object_id=" << payload.object_id
                      << ", path=" << pending_path << ", tag_time=" << payload.tag_time
                      << std::endl;
            return;
        }

        try {
            const fs::path moved = move_file_to_dir(pending_path, fs::path(kEventImageFailedDir));
            std::cerr << "[main.cpp] [RFID_IMAGE_DELETE] failed to delete pending image, moved to"
                      << " failed dir: object_id=" << payload.object_id << ", moved=" << moved
                      << ", err=" << remove_ec.message() << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[main.cpp] [RFID_IMAGE_DELETE] failed: object_id=" << payload.object_id
                      << ", path=" << pending_path << ", err=" << e.what() << std::endl;
        }
        return;
    }

    try {
        const fs::path kept = move_file_to_dir(pending_path, fs::path(kEventImageFraudDir));
        std::cout << "[main.cpp] [RFID_IMAGE_KEEP] object_id=" << payload.object_id
                  << ", from=" << pending_path << ", to=" << kept
                  << ", tag_time=" << payload.tag_time << std::endl;
    } catch (const std::exception& keep_err) {
        try {
            const fs::path failed = move_file_to_dir(pending_path, fs::path(kEventImageFailedDir));
            std::cerr << "[main.cpp] [RFID_IMAGE_KEEP] failed to keep in fraud dir, moved to failed"
                      << ": object_id=" << payload.object_id << ", moved=" << failed
                      << ", err=" << keep_err.what() << std::endl;
        } catch (const std::exception& failed_err) {
            std::cerr << "[main.cpp] [RFID_IMAGE_KEEP] failed: object_id=" << payload.object_id
                      << ", path=" << pending_path << ", err=" << keep_err.what()
                      << ", failed_err=" << failed_err.what() << std::endl;
        }
    }
}
