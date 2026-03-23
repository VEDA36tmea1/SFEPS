#include "rfid_image_pipeline.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <stdexcept>
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
constexpr int kCaptureAckTimeoutMs = 700;

struct EventImageRegistry {
    std::mutex mutex;
    std::unordered_map<std::string, fs::path> pending_by_object_id;
};

enum class TriggerRequestResult {
    kOk = 0,
    kTimeout = 1,
    kAckFail = 2,
    kTransportFail = 3,
};

struct TriggerAckResult {
    TriggerRequestResult result = TriggerRequestResult::kTransportFail;
    std::string req_id;
    std::string path;
    std::string err;
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

std::vector<std::string> split_pipe(const std::string& raw) {
    std::vector<std::string> out;
    std::string token;
    std::istringstream iss(raw);
    while (std::getline(iss, token, '|')) {
        out.push_back(token);
    }
    return out;
}

std::map<std::string, std::string> parse_ack_kv(const std::string& line) {
    std::map<std::string, std::string> kv;
    const std::vector<std::string> tokens = split_pipe(line);
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];
        const std::size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        kv[t.substr(0, eq)] = t.substr(eq + 1);
    }
    return kv;
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

TriggerAckResult request_camera_capture(const std::string& req_id,
                                        const std::string& object_id,
                                        const std::string& tag_time,
                                        const fs::path& out_path) {
    TriggerAckResult out;
    out.req_id = req_id;

    const int sock_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        out.result = TriggerRequestResult::kTransportFail;
        out.err = "socket() failed";
        return out;
    }

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kCameraTriggerSocketPath, sizeof(addr.sun_path) - 1);
    if (::connect(sock_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        out.result = TriggerRequestResult::kTransportFail;
        out.err = "connect() failed";
        ::close(sock_fd);
        return out;
    }

    const std::string request =
        "CAPTURE_REQ|REQ_ID=" + req_id +
        "|OBJECT_ID=" + object_id +
        "|TAG=" + tag_time +
        "|OUT=" + out_path.string() + "\n";
    if (!send_all(sock_fd, request)) {
        out.result = TriggerRequestResult::kTransportFail;
        out.err = "send() failed";
        ::close(sock_fd);
        return out;
    }

    pollfd pfd {};
    pfd.fd = sock_fd;
    pfd.events = POLLIN;
    const int poll_ret = ::poll(&pfd, 1, kCaptureAckTimeoutMs);
    if (poll_ret == 0) {
        out.result = TriggerRequestResult::kTimeout;
        out.err = "ack timeout";
        ::close(sock_fd);
        return out;
    }
    if (poll_ret < 0) {
        out.result = TriggerRequestResult::kTransportFail;
        out.err = "poll() failed";
        ::close(sock_fd);
        return out;
    }

    std::string line;
    char ch = '\0';
    while (true) {
        const ssize_t n = ::read(sock_fd, &ch, 1);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            out.result = TriggerRequestResult::kTransportFail;
            out.err = "read() failed";
            ::close(sock_fd);
            return out;
        }
        if (ch == '\n') break;
        line.push_back(ch);
        if (line.size() >= 4096) break;
    }
    ::close(sock_fd);

    if (line.rfind("CAPTURE_ACK|", 0) != 0) {
        out.result = TriggerRequestResult::kAckFail;
        out.err = "invalid ack prefix";
        return out;
    }

    const std::map<std::string, std::string> kv = parse_ack_kv(line);
    const auto req_it = kv.find("REQ_ID");
    if (req_it == kv.end() || req_it->second != req_id) {
        out.result = TriggerRequestResult::kAckFail;
        out.err = "req_id mismatch";
        return out;
    }
    const auto ok_it = kv.find("OK");
    if (ok_it == kv.end()) {
        out.result = TriggerRequestResult::kAckFail;
        out.err = "missing OK";
        return out;
    }
    if (ok_it->second != "1") {
        out.result = TriggerRequestResult::kAckFail;
        const auto err_it = kv.find("ERR");
        out.err = (err_it != kv.end()) ? err_it->second : "capture failed";
        return out;
    }

    const auto path_it = kv.find("PATH");
    if (path_it == kv.end() || path_it->second.empty()) {
        out.result = TriggerRequestResult::kAckFail;
        out.err = "missing PATH";
        return out;
    }
    out.path = path_it->second;
    out.result = TriggerRequestResult::kOk;
    return out;
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

    std::cout << "[main.cpp] [CAM_TRIGGER_SEND] object_id=" << object_id
              << ", tag_time=" << tag_time << ", req_id=" << req_id
              << ", out=" << out_path << std::endl;

    TriggerAckResult ack = request_camera_capture(req_id, object_id, tag_time, out_path);
    if (ack.result == TriggerRequestResult::kTimeout) {
        std::cerr << "[main.cpp] [CAM_TRIGGER_TIMEOUT] object_id=" << object_id
                  << ", req_id=" << req_id << ", timeout_ms=" << kCaptureAckTimeoutMs
                  << std::endl;
        return;
    }
    if (ack.result == TriggerRequestResult::kTransportFail) {
        std::cerr << "[main.cpp] [CAM_TRIGGER_ACK_FAIL] object_id=" << object_id
                  << ", req_id=" << req_id << ", reason=" << ack.err << std::endl;
        return;
    }
    if (ack.result == TriggerRequestResult::kAckFail) {
        std::cerr << "[main.cpp] [CAM_TRIGGER_ACK_FAIL] object_id=" << object_id
                  << ", req_id=" << req_id << ", reason=" << ack.err << std::endl;
        return;
    }

    const fs::path ack_path(ack.path);
    if (!is_pending_path_safe(ack_path) || !fs::exists(ack_path) || !fs::is_regular_file(ack_path)) {
        std::cerr << "[main.cpp] [CAM_TRIGGER_ACK_FAIL] object_id=" << object_id
                  << ", req_id=" << req_id << ", reason=invalid ack path, path=" << ack_path
                  << std::endl;
        return;
    }

    auto& registry = event_image_registry();
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        const auto existing = registry.pending_by_object_id.find(object_id);
        if (existing != registry.pending_by_object_id.end()) {
            std::error_code remove_ec;
            fs::remove(existing->second, remove_ec);
        }
        registry.pending_by_object_id[object_id] = ack_path;
    }

    std::cout << "[main.cpp] [CAM_TRIGGER_ACK_OK] object_id=" << object_id
              << ", req_id=" << req_id << ", path=" << ack_path << std::endl;
}

bool finalize_outline_image_for_object(
    const AnalyticsProcessor::OutlineDecisionPayload& payload,
    FinalizedFraudImageInfo* out_fraud_image_info) {
    if (out_fraud_image_info != nullptr) {
        *out_fraud_image_info = FinalizedFraudImageInfo {};
    }
    if (payload.object_id.empty()) return false;

    auto& registry = event_image_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);

    const auto it = registry.pending_by_object_id.find(payload.object_id);
    if (it == registry.pending_by_object_id.end()) {
        std::cout << "[main.cpp] ["
                  << (payload.is_fraud ? "RFID_IMAGE_KEEP" : "RFID_IMAGE_DELETE")
                  << "] no pending image: object_id=" << payload.object_id
                  << ", tag_time=" << payload.tag_time << std::endl;
        return false;
    }

    const fs::path pending_path = it->second;
    registry.pending_by_object_id.erase(it);

    if (!fs::exists(pending_path)) {
        std::cout << "[main.cpp] ["
                  << (payload.is_fraud ? "RFID_IMAGE_KEEP" : "RFID_IMAGE_DELETE")
                  << "] pending image missing on disk: object_id=" << payload.object_id
                  << ", path=" << pending_path << ", tag_time=" << payload.tag_time
                  << std::endl;
        return false;
    }

    if (!payload.is_fraud) {
        std::error_code remove_ec;
        if (fs::remove(pending_path, remove_ec)) {
            std::cout << "[main.cpp] [RFID_IMAGE_DELETE] object_id=" << payload.object_id
                      << ", path=" << pending_path << ", tag_time=" << payload.tag_time
                      << std::endl;
            return false;
        }

        try {
            const fs::path moved = move_file_to_dir(pending_path, fs::path(kEventImageFailedDir));
            std::cerr << "[main.cpp] [RFID_IMAGE_DELETE] 대기 이미지 삭제 실패, 이동 경로="
                      << " 실패 디렉터리: object_id=" << payload.object_id << ", moved=" << moved
                      << ", 오류=" << remove_ec.message() << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[main.cpp] [RFID_IMAGE_DELETE] 실패: object_id=" << payload.object_id
                      << ", path=" << pending_path << ", 오류=" << e.what() << std::endl;
        }
        return false;
    }

    try {
        const fs::path kept = move_file_to_dir(pending_path, fs::path(kEventImageFraudDir));
        std::cout << "[main.cpp] [RFID_IMAGE_KEEP] object_id=" << payload.object_id
                  << ", from=" << pending_path << ", to=" << kept
                  << ", tag_time=" << payload.tag_time << std::endl;
        if (out_fraud_image_info != nullptr) {
            out_fraud_image_info->object_id = payload.object_id;
            out_fraud_image_info->tag_time = payload.tag_time;
            out_fraud_image_info->filename = kept.filename().string();
            out_fraud_image_info->absolute_path = kept.string();
        }
        return true;
    } catch (const std::exception& keep_err) {
        try {
            const fs::path failed = move_file_to_dir(pending_path, fs::path(kEventImageFailedDir));
            std::cerr << "[main.cpp] [RFID_IMAGE_KEEP] 사기 디렉터리 보관 실패, 실패 디렉터리로 이동"
                      << ": object_id=" << payload.object_id << ", moved=" << failed
                      << ", 오류=" << keep_err.what() << std::endl;
        } catch (const std::exception& failed_err) {
            std::cerr << "[main.cpp] [RFID_IMAGE_KEEP] 실패: object_id=" << payload.object_id
                      << ", path=" << pending_path << ", 오류=" << keep_err.what()
                      << ", 실패_오류=" << failed_err.what() << std::endl;
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
