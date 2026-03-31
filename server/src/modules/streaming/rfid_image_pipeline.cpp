#include "rfid_image_pipeline.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "recorder.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kEventImageBaseDir = "/home/iam/SFEPS/event_images";
constexpr const char* kEventImagePendingDir = "/home/iam/SFEPS/event_images/pending";
constexpr const char* kEventImageFraudDir = "/home/iam/SFEPS/event_images/fraud";
constexpr const char* kEventImageFailedDir = "/home/iam/SFEPS/event_images/failed";
constexpr const char* kCameraTriggerSocketPath = "/tmp/sfeps_camera_trigger.sock";
constexpr auto kPendingImageReadyWait = std::chrono::milliseconds(1500);
constexpr auto kPendingImageReadyPoll = std::chrono::milliseconds(50);

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

std::string sanitize_tag_for_filename(const std::string& raw) {
    if (raw.empty()) return "Unknown";

    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z')) {
            out.push_back(static_cast<char>(c));
        }
    }
    return out.empty() ? "Unknown" : out;
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

std::string build_req_id() {
    static const std::uint64_t kStartMs = unix_epoch_ms_now();
    static std::atomic<std::uint64_t> seq {0};
    const std::uint64_t id = ++seq;
    return "R" + std::to_string(kStartMs) + "_" + std::to_string(id);
}

bool send_all(int fd, const std::string& msg) {
    std::size_t sent = 0;
    while (sent < msg.size()) {
        const ssize_t n = ::write(fd, msg.data() + sent, msg.size() - sent);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool is_existing_regular_file(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec) && !ec && fs::is_regular_file(path, ec) && !ec;
}

bool wait_for_regular_file(const fs::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + kPendingImageReadyWait;
    while (true) {
        if (is_existing_regular_file(path)) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(kPendingImageReadyPoll);
    }
}

bool is_jpeg_image_path(const fs::path& path) {
    const std::string ext = path.extension().string();
    std::string lower;
    lower.reserve(ext.size());
    for (unsigned char c : ext) {
        lower.push_back(static_cast<char>(std::tolower(c)));
    }
    return lower == ".jpg" || lower == ".jpeg";
}

bool is_pending_path_safe(const fs::path& path) {
    std::error_code ec;
    const fs::path base = fs::weakly_canonical(fs::path(kEventImagePendingDir), ec);
    if (ec) return false;
    ec.clear();
    const fs::path target = fs::weakly_canonical(path, ec);
    if (ec) return false;

    auto b = base.begin();
    auto t = target.begin();
    for (; b != base.end() && t != target.end(); ++b, ++t) {
        if (*b != *t) return false;
    }
    return b == base.end();
}

void erase_registry_path_if_matches(const std::string& object_id, const fs::path& expected_path) {
    if (object_id.empty() || expected_path.empty()) return;

    auto& registry = event_image_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto it = registry.pending_by_object_id.find(object_id);
    if (it == registry.pending_by_object_id.end()) return;
    if (it->second != expected_path) return;
    registry.pending_by_object_id.erase(it);
}

bool request_camera_capture(const std::string& req_id,
                            const std::string& object_id,
                            const std::string& tag_time,
                            const fs::path& out_path,
                            std::string& err) {
    const int sock_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        err = "socket() failed";
        return false;
    }

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kCameraTriggerSocketPath, sizeof(addr.sun_path) - 1);
    if (::connect(sock_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        err = "connect() failed";
        ::close(sock_fd);
        return false;
    }

    const std::string request =
        "CAPTURE_REQ|REQ_ID=" + req_id +
        "|OBJECT_ID=" + object_id +
        "|TAG=" + tag_time +
        "|OUT=" + out_path.string() + "\n";
    if (!send_all(sock_fd, request)) {
        err = "send() failed";
        ::close(sock_fd);
        return false;
    }
    ::close(sock_fd);
    return true;
}

std::vector<fs::path> collect_pending_image_candidates_for_object(const std::string& object_id) {
    std::vector<fs::path> candidates;
    const std::string match_token = "_" + sanitize_filename_token(object_id) + "_";
    std::error_code ec;
    if (!fs::exists(kEventImagePendingDir, ec) || ec) {
        return candidates;
    }

    for (const auto& entry : fs::directory_iterator(kEventImagePendingDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const fs::path path = entry.path();
        if (!is_pending_path_safe(path) || !is_jpeg_image_path(path)) continue;
        const std::string filename = path.filename().string();
        if (filename.find(match_token) == std::string::npos) continue;
        candidates.push_back(path);
    }

    std::sort(candidates.begin(), candidates.end(), [](const fs::path& lhs, const fs::path& rhs) {
        std::error_code lhs_ec;
        std::error_code rhs_ec;
        const auto lhs_time = fs::last_write_time(lhs, lhs_ec);
        const auto rhs_time = fs::last_write_time(rhs, rhs_ec);
        if (lhs_ec && rhs_ec) return lhs.string() < rhs.string();
        if (lhs_ec) return false;
        if (rhs_ec) return true;
        if (lhs_time == rhs_time) return lhs.string() < rhs.string();
        return lhs_time > rhs_time;
    });
    return candidates;
}

void dedupe_paths(std::vector<fs::path>& paths) {
    std::unordered_set<std::string> seen;
    std::vector<fs::path> deduped;
    deduped.reserve(paths.size());
    for (const auto& path : paths) {
        const std::string key = path.string();
        if (seen.insert(key).second) {
            deduped.push_back(path);
        }
    }
    paths.swap(deduped);
}

void prune_pending_images(const std::string& object_id,
                          const std::vector<fs::path>& paths,
                          const char* reason_key) {
    (void)object_id;
    (void)reason_key;
    for (const auto& path : paths) {
        if (!is_existing_regular_file(path)) continue;
        std::error_code remove_ec;
        fs::remove(path, remove_ec);
    }
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

void snapshot_rfid_image_for_object(const std::string& object_id, const std::string& tag_time) {
    if (object_id.empty()) return;

    const std::string safe_object_id = sanitize_filename_token(object_id);
    const std::string safe_tag = sanitize_tag_for_filename(tag_time);
    const std::string req_id = build_req_id();
    const fs::path out_path = make_unique_path(
        fs::path(kEventImagePendingDir) /
        ("capture_" + safe_tag + "_" + safe_object_id + "_" + sanitize_filename_token(req_id) +
         ".jpg"));

    std::string send_err;
    if (!request_camera_capture(req_id, object_id, tag_time, out_path, send_err)) {
        return;
    }

    auto& registry = event_image_registry();
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        registry.pending_by_object_id[object_id] = out_path;
    }

}

bool finalize_outline_image_for_object(
    const AnalyticsProcessor::OutlineDecisionPayload& payload,
    FinalizedFraudImageInfo* out_fraud_image_info) {
    if (out_fraud_image_info != nullptr) {
        *out_fraud_image_info = FinalizedFraudImageInfo {};
    }
    if (payload.object_id.empty()) return false;

    auto& registry = event_image_registry();
    fs::path registry_path;
    bool had_registry_path = false;
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        const auto it = registry.pending_by_object_id.find(payload.object_id);
        if (it != registry.pending_by_object_id.end()) {
            registry_path = it->second;
            had_registry_path = true;
        }
    }

    if (had_registry_path && !is_existing_regular_file(registry_path)) {
        wait_for_regular_file(registry_path);
    }

    std::vector<fs::path> object_pending_paths =
        collect_pending_image_candidates_for_object(payload.object_id);
    fs::path pending_path;
    bool used_fallback = false;

    if (had_registry_path && is_existing_regular_file(registry_path)) {
        pending_path = registry_path;
    } else if (!object_pending_paths.empty()) {
        pending_path = object_pending_paths.front();
        used_fallback = true;
    }

    if (pending_path.empty()) {
        return false;
    }

    erase_registry_path_if_matches(payload.object_id, registry_path);
    (void)used_fallback;

    std::vector<fs::path> duplicate_paths;
    duplicate_paths.reserve(object_pending_paths.size() + 1);
    for (const auto& candidate : object_pending_paths) {
        if (candidate != pending_path) {
            duplicate_paths.push_back(candidate);
        }
    }
    if (had_registry_path && !registry_path.empty() && registry_path != pending_path &&
        is_existing_regular_file(registry_path)) {
        duplicate_paths.push_back(registry_path);
    }
    dedupe_paths(duplicate_paths);

    if (!payload.is_fraud) {
        std::error_code remove_ec;
        if (fs::remove(pending_path, remove_ec)) {
            prune_pending_images(payload.object_id, duplicate_paths, "RFID_IMAGE_FALLBACK_PRUNE");
            return false;
        }

        try {
            move_file_to_dir(pending_path, fs::path(kEventImageFailedDir));
        } catch (const std::exception&) {
        }
        prune_pending_images(payload.object_id, duplicate_paths, "RFID_IMAGE_FALLBACK_PRUNE");
        return false;
    }

    try {
        const fs::path kept = move_file_to_dir(pending_path, fs::path(kEventImageFraudDir));
        if (out_fraud_image_info != nullptr) {
            out_fraud_image_info->object_id = payload.object_id;
            out_fraud_image_info->tag_time = payload.tag_time;
            out_fraud_image_info->filename = kept.filename().string();
            out_fraud_image_info->absolute_path = kept.string();
        }
        prune_pending_images(payload.object_id, duplicate_paths, "RFID_IMAGE_FALLBACK_PRUNE");
        return true;
    } catch (const std::exception&) {
        try {
            move_file_to_dir(pending_path, fs::path(kEventImageFailedDir));
        } catch (const std::exception&) {
        }
    }
    return false;
}

const char* fraud_image_directory_path() {
    return kEventImageFraudDir;
}

const char* pending_image_directory_path() {
    return kEventImagePendingDir;
}
