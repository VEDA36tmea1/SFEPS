// camera_client.cpp
// Server-triggered capture worker:
// - capture thread keeps latest frame
// - trigger listener receives CAPTURE_REQ over local UDS
// - pipeline worker writes requested output path

#include <opencv2/opencv.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <unistd.h>

#include "../inc/img_processing.h"

#define LIVE_CAMERA_MODE 1

namespace {

constexpr const char* kTriggerSocketPath = "/tmp/sfeps_camera_trigger.sock";
constexpr const char* kPendingBaseDir = "/home/iam/SFEPS/event_images/pending";
constexpr const char* kShutdownAbortReason = "SHUTDOWN";
constexpr const char* kRawHelperSocketPath = "/tmp/sfeps_raw_helper.sock";
constexpr int kRequestQueueMax = 5;
constexpr int kClientReadTimeoutMs = 1000;
constexpr int kRawHelperTimeoutMs = 10000;
constexpr int kExposureTimeUs = 8000;
constexpr float kAnalogueGain = 1.0f;

std::atomic<bool> g_running {true};
std::atomic<bool> g_shutdown_started {false};
std::atomic<bool> g_signal_thread_stop {false};
std::atomic<int> g_listener_fd {-1};
std::atomic<int> g_active_client_fd {-1};
std::atomic<std::uint64_t> g_shutdown_started_ms {0};
std::atomic<cv::VideoCapture*> g_capture_device {nullptr};
bool g_raw_mode = false;
bool g_use_native_raw_helper = false;
std::string g_active_pipeline_name;
pid_t g_raw_helper_pid = -1;

std::mutex g_raw_mutex;
std::queue<cv::Mat> g_raw_queue;

struct CaptureRequest {
    std::string req_id;
    std::string object_id;
    std::string tag;
    std::string out_path;
};

std::mutex g_request_mutex;
std::condition_variable g_request_cv;
std::queue<CaptureRequest> g_request_queue;

std::vector<std::string> split_pipe(const std::string& raw) {
    std::vector<std::string> out;
    std::string token;
    std::istringstream iss(raw);
    while (std::getline(iss, token, '|')) {
        out.push_back(token);
    }
    return out;
}

std::string mat_type_string(int type) {
    const int depth = CV_MAT_DEPTH(type);
    const int channels = 1 + (type >> CV_CN_SHIFT);

    std::string depth_str;
    switch (depth) {
        case CV_8U: depth_str = "8U"; break;
        case CV_8S: depth_str = "8S"; break;
        case CV_16U: depth_str = "16U"; break;
        case CV_16S: depth_str = "16S"; break;
        case CV_32S: depth_str = "32S"; break;
        case CV_32F: depth_str = "32F"; break;
        case CV_64F: depth_str = "64F"; break;
        default: depth_str = "User"; break;
    }

    return "CV_" + depth_str + "C" + std::to_string(channels);
}

std::map<std::string, std::string> parse_kv_file(const std::string& path) {
    std::map<std::string, std::string> kv;
    std::ifstream ifs(path.c_str());
    std::string line;
    while (std::getline(ifs, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

std::map<std::string, std::string> parse_kv_line(const std::string& line) {
    std::map<std::string, std::string> kv;
    const std::vector<std::string> tokens = split_pipe(line);
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        const std::size_t eq = tokens[i].find('=');
        if (eq == std::string::npos) continue;
        kv[tokens[i].substr(0, eq)] = tokens[i].substr(eq + 1);
    }
    return kv;
}

bool read_line_with_timeout(int fd, int timeout_ms, std::string& out_line) {
    out_line.clear();
    pollfd pfd {};
    pfd.fd = fd;
    pfd.events = POLLIN;

    const int poll_ret = ::poll(&pfd, 1, timeout_ms);
    if (poll_ret <= 0) return false;
    if ((pfd.revents & POLLIN) == 0) return false;

    char ch = '\0';
    while (true) {
        const ssize_t n = ::read(fd, &ch, 1);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (ch == '\n') break;
        out_line.push_back(ch);
        if (out_line.size() >= 8192) break;
    }
    return !out_line.empty();
}

bool is_safe_pending_output(const std::string& path_str) {
    if (path_str.empty() || path_str[0] != '/') return false;
    if (path_str.find('\n') != std::string::npos) return false;
    if (path_str.find('\r') != std::string::npos) return false;
    if (path_str.find("..") != std::string::npos) return false;

    const std::string base_prefix = std::string(kPendingBaseDir) + "/";
    if (path_str.rfind(base_prefix, 0) != 0) return false;
    return true;
}

bool ensure_dir_exists(const std::string& dir_path) {
    if (dir_path.empty()) return false;

    std::string current;
    if (dir_path[0] == '/') current = "/";

    std::istringstream iss(dir_path);
    std::string part;
    while (std::getline(iss, part, '/')) {
        if (part.empty()) continue;
        if (!current.empty() && current.back() != '/') current += "/";
        current += part;

        if (::mkdir(current.c_str(), 0755) != 0) {
            if (errno != EEXIST) return false;
        }
    }
    return true;
}

std::string parent_dir_of(const std::string& path) {
    const std::size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return std::string();
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

std::uint64_t monotonic_ms_now() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

const char* signal_name(int sig) {
    switch (sig) {
        case SIGINT:
            return "SIGINT";
        case SIGTERM:
            return "SIGTERM";
        default:
            return "UNKNOWN";
    }
}

void close_fd_slot(std::atomic<int>& fd_slot) {
    const int fd = fd_slot.exchange(-1);
    if (fd >= 0) ::close(fd);
}

std::size_t clear_pending_requests() {
    std::lock_guard<std::mutex> lock(g_request_mutex);
    const std::size_t dropped = g_request_queue.size();
    while (!g_request_queue.empty()) g_request_queue.pop();
    return dropped;
}

void clear_latest_frames() {
    std::lock_guard<std::mutex> lock(g_raw_mutex);
    while (!g_raw_queue.empty()) g_raw_queue.pop();
}

void stopNativeRawHelper();

bool read_binary_file(const std::string& path, std::vector<unsigned char>& out) {
    std::ifstream ifs(path.c_str(), std::ios::binary);
    if (!ifs) return false;
    ifs.seekg(0, std::ios::end);
    const std::streamoff size = ifs.tellg();
    if (size < 0) return false;
    ifs.seekg(0, std::ios::beg);
    out.assign(static_cast<std::size_t>(size), 0);
    return ifs.read(reinterpret_cast<char*>(out.data()), size).good() || ifs.eof();
}

void release_capture_device() {
    cv::VideoCapture* capture = g_capture_device.load();
    if (capture == nullptr) return;

    if (capture->isOpened()) {
        std::cout << "[camera] shutdown: releasing capture pipeline" << std::endl;
        capture->release();
    }
}

void requestShutdown(const char* reason) {
    if (g_shutdown_started.exchange(true)) return;

    const std::uint64_t started_ms = monotonic_ms_now();
    g_shutdown_started_ms.store(started_ms);
    g_running = false;

    const std::size_t dropped_requests = clear_pending_requests();
    clear_latest_frames();

    std::cout << "[camera] shutdown start: reason="
              << (reason != nullptr ? reason : "UNKNOWN")
              << ", dropped_requests=" << dropped_requests << std::endl;

    close_fd_slot(g_active_client_fd);
    close_fd_slot(g_listener_fd);
    ::unlink(kTriggerSocketPath);
    stopNativeRawHelper();
    g_request_cv.notify_all();
    release_capture_device();
}

bool decodeRaw10Unpacked(const std::vector<unsigned char>& bytes,
                         int width,
                         int height,
                         int stride,
                         cv::Mat& out,
                         std::string& err) {
    if (stride < width * 2) {
        err = "unpacked RAW stride too small";
        return false;
    }
    if (bytes.size() < static_cast<std::size_t>(stride) * height) {
        err = "unpacked RAW buffer too small";
        return false;
    }

    out.create(height, width, CV_16UC1);
    for (int y = 0; y < height; ++y) {
        const unsigned char* src = bytes.data() + static_cast<std::size_t>(y) * stride;
        uint16_t* dst = out.ptr<uint16_t>(y);
        for (int x = 0; x < width; ++x) {
            const int offset = x * 2;
            dst[x] = static_cast<uint16_t>(src[offset] | (src[offset + 1] << 8));
        }
    }
    return true;
}

bool decodeRaw10PackedCSI2(const std::vector<unsigned char>& bytes,
                           int width,
                           int height,
                           int stride,
                           cv::Mat& out,
                           std::string& err) {
    const int min_stride = (width * 5 + 3) / 4;
    if (stride < min_stride) {
        err = "packed RAW stride too small";
        return false;
    }
    if (bytes.size() < static_cast<std::size_t>(stride) * height) {
        err = "packed RAW buffer too small";
        return false;
    }

    out.create(height, width, CV_16UC1);
    for (int y = 0; y < height; ++y) {
        const unsigned char* src = bytes.data() + static_cast<std::size_t>(y) * stride;
        uint16_t* dst = out.ptr<uint16_t>(y);
        int x = 0;
        int byte_idx = 0;
        while (x + 3 < width) {
            const unsigned char b0 = src[byte_idx + 0];
            const unsigned char b1 = src[byte_idx + 1];
            const unsigned char b2 = src[byte_idx + 2];
            const unsigned char b3 = src[byte_idx + 3];
            const unsigned char b4 = src[byte_idx + 4];
            dst[x + 0] = static_cast<uint16_t>((b0 << 2) | ((b4 >> 0) & 0x03));
            dst[x + 1] = static_cast<uint16_t>((b1 << 2) | ((b4 >> 2) & 0x03));
            dst[x + 2] = static_cast<uint16_t>((b2 << 2) | ((b4 >> 4) & 0x03));
            dst[x + 3] = static_cast<uint16_t>((b3 << 2) | ((b4 >> 6) & 0x03));
            x += 4;
            byte_idx += 5;
        }
        if (x != width) {
            err = "packed RAW width not divisible by 4";
            return false;
        }
    }
    return true;
}

bool loadRawFrameFromFiles(const std::string& raw_path,
                           const std::string& meta_path,
                           cv::Mat& out,
                           std::string& err) {
    const std::map<std::string, std::string> meta = parse_kv_file(meta_path);
    const auto fmt_it = meta.find("FORMAT");
    const auto width_it = meta.find("WIDTH");
    const auto height_it = meta.find("HEIGHT");
    const auto stride_it = meta.find("STRIDE");
    if (fmt_it == meta.end() || width_it == meta.end() ||
        height_it == meta.end() || stride_it == meta.end()) {
        err = "RAW metadata incomplete";
        return false;
    }

    const std::string format = fmt_it->second;
    const int width = std::atoi(width_it->second.c_str());
    const int height = std::atoi(height_it->second.c_str());
    const int stride = std::atoi(stride_it->second.c_str());

    std::vector<unsigned char> bytes;
    if (!read_binary_file(raw_path, bytes)) {
        err = "RAW binary read failed";
        return false;
    }

    std::cout << "[camera] raw helper frame: format=" << format
              << ", size=" << width << "x" << height
              << ", stride=" << stride << std::endl;

    if (format == "SBGGR10" || format == "SRGGB10" ||
        format == "SGBRG10" || format == "SGRBG10") {
        return decodeRaw10Unpacked(bytes, width, height, stride, out, err);
    }
    if (format == "SBGGR10_CSI2P" || format == "SRGGB10_CSI2P" ||
        format == "SGBRG10_CSI2P" || format == "SGRBG10_CSI2P") {
        return decodeRaw10PackedCSI2(bytes, width, height, stride, out, err);
    }

    err = "unsupported RAW format: " + format;
    return false;
}

bool captureRawFrameWithHelper(cv::Mat& frame, std::string& err) {
    const std::string base = "/tmp/sfeps_raw_" + std::to_string(::getpid()) + "_" +
                             std::to_string(monotonic_ms_now());
    const std::string raw_path = base + ".bin";
    const std::string meta_path = base + ".txt";

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        err = "native RAW helper socket create failed";
        return false;
    }

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kRawHelperSocketPath, sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        err = "native RAW helper connect failed";
        ::close(fd);
        return false;
    }

    const std::string request = "CAPTURE|RAW=" + raw_path + "|META=" + meta_path + "\n";
    const ssize_t write_rc = ::write(fd, request.c_str(), request.size());
    if (write_rc < 0) {
        err = "native RAW helper request write failed";
        ::close(fd);
        return false;
    }

    std::string response;
    const bool read_ok = read_line_with_timeout(fd, kRawHelperTimeoutMs, response);
    ::close(fd);
    if (!read_ok) {
        err = "native RAW helper response timeout";
        return false;
    }
    if (response != "OK") {
        err = response.rfind("ERR|", 0) == 0 ? response.substr(4) : response;
        return false;
    }

    const bool ok = loadRawFrameFromFiles(raw_path, meta_path, frame, err);
    std::remove(raw_path.c_str());
    std::remove(meta_path.c_str());
    return ok;
}

bool block_termination_signals(sigset_t& signal_set) {
    ::sigemptyset(&signal_set);
    ::sigaddset(&signal_set, SIGINT);
    ::sigaddset(&signal_set, SIGTERM);
    const int rc = ::pthread_sigmask(SIG_BLOCK, &signal_set, nullptr);
    if (rc != 0) {
        std::cerr << "[camera] pthread_sigmask failed: rc=" << rc << std::endl;
        return false;
    }
    return true;
}

void signalWaitThread(sigset_t signal_set) {
    while (true) {
        int sig = 0;
        const int rc = ::sigwait(&signal_set, &sig);
        if (rc != 0) {
            std::cerr << "[camera] sigwait failed: rc=" << rc << std::endl;
            continue;
        }

        if (g_signal_thread_stop.load()) break;

        requestShutdown(signal_name(sig));
        break;
    }

    std::cout << "[camera] signal watcher exit" << std::endl;
}

void stopSignalThread(std::thread& signal_thread) {
    g_signal_thread_stop = true;
    if (!signal_thread.joinable()) return;

    const int rc = ::pthread_kill(signal_thread.native_handle(), SIGTERM);
    (void)rc;
    signal_thread.join();
}

#if LIVE_CAMERA_MODE
void captureThreadFunc(cv::VideoCapture& cap) {
    while (g_running) {
        if (!cap.grab()) {
            if (!g_running || !cap.isOpened()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (!g_running) break;

        cv::Mat frame;
        if (!cap.retrieve(frame)) {
            if (!g_running || !cap.isOpened()) break;
            continue;
        }

        if (!g_running) break;
        if (frame.empty()) continue;

        std::lock_guard<std::mutex> lock(g_raw_mutex);
        while (!g_raw_queue.empty()) g_raw_queue.pop();
        g_raw_queue.push(std::move(frame));
    }

    if (!g_shutdown_started.load()) {
        requestShutdown("capture_thread_stopped");
    }
}
#endif

bool fetchLatestFrame(cv::Mat& frame, std::string& err) {
    if (g_use_native_raw_helper) {
        return captureRawFrameWithHelper(frame, err);
    }

    std::lock_guard<std::mutex> lock(g_raw_mutex);
    if (!g_raw_queue.empty()) frame = g_raw_queue.front().clone();
    if (frame.empty()) err = "NO_FRAME";
    return !frame.empty();
}

bool runFullPipeline(const cv::Mat& frame,
                     bool raw_mode,
                     const std::string& out_path,
                     std::string& err) {
    const std::string debug_dir = "/home/iam/SFEPS/Camera/image_processing/isp_debug/";

    if (!g_running) {
        err = kShutdownAbortReason;
        return false;
    }

    if (frame.empty()) {
        err = "empty frame";
        return false;
    }

    if (!ensure_dir_exists(debug_dir)) {
        std::cerr << "[camera] isp debug dir create failed: " << debug_dir << std::endl;
    }

    std::cout << "[camera] frame info: pipeline=" << g_active_pipeline_name
              << ", raw_mode=" << (raw_mode ? "true" : "false")
              << ", size=" << frame.cols << "x" << frame.rows
              << ", type=" << mat_type_string(frame.type())
              << ", depth=" << frame.depth()
              << ", channels=" << frame.channels() << std::endl;

    cv::Mat raw_vis;
    double min_val, max_val;
    cv::minMaxLoc(frame, &min_val, &max_val);
    frame.convertTo(raw_vis, CV_8UC1, 255.0 / (max_val - min_val), -min_val * 255.0 / (max_val - min_val));
    cv::imwrite(debug_dir + "1_raw_capture.png", raw_vis);

    cv::Mat isp_out;
    if (raw_mode) {
        if (frame.type() != CV_16UC1) {
            std::cerr << "[camera] RAW pipeline opened but Bayer histogram path skipped: "
                      << "expected=CV_16UC1, actual=" << mat_type_string(frame.type())
                      << std::endl;
            isp_out = frame;
        } else {
            isp_out = runPureISP_withHistograms(frame, debug_dir);
            cv::imwrite(debug_dir + "2_pure_isp_out.png", isp_out);
        }
    } else {
        std::cerr << "[camera] Bayer histogram path skipped: BGR fallback mode is active"
                  << std::endl;
        isp_out = frame;
    }

    if (!g_running) {
        err = kShutdownAbortReason;
        return false;
    }

    cv::Mat tuning_view;
    cv::Mat best_frame = processISPAndGetBest(isp_out, tuning_view);
    cv::imwrite(debug_dir + "3_tuning_viewer.png", tuning_view);

    if (!g_running) {
        err = kShutdownAbortReason;
        return false;
    }

    const std::string out_dir = parent_dir_of(out_path);
    if (out_dir.empty() || !ensure_dir_exists(out_dir)) {
        err = "mkdir failed";
        return false;
    }

    if (!g_running) {
        err = kShutdownAbortReason;
        return false;
    }

    const std::string temp_out_path = out_path + ".tmp.jpg";
    std::remove(temp_out_path.c_str());

    if (!cv::imwrite(temp_out_path, best_frame)) {
        err = "cv::imwrite failed";
        return false;
    }

    if (std::rename(temp_out_path.c_str(), out_path.c_str()) != 0) {
        std::remove(temp_out_path.c_str());
        err = "rename failed";
        return false;
    }
    return true;
}

void pipelineWorkerThread() {
    while (true) {
        CaptureRequest req;
        {
            std::unique_lock<std::mutex> lock(g_request_mutex);
            g_request_cv.wait(lock, [] { return !g_running || !g_request_queue.empty(); });
            if (!g_running) break;
            req = std::move(g_request_queue.front());
            g_request_queue.pop();
        }

        if (!g_running) break;

        cv::Mat snapshot;
        std::string capture_err;
        if (!fetchLatestFrame(snapshot, capture_err)) {
            std::cerr << "[camera] capture failed: req_id=" << req.req_id
                      << ", object_id=" << req.object_id
                      << ", reason=" << (capture_err.empty() ? "NO_FRAME" : capture_err)
                      << std::endl;
            continue;
        }

        if (!g_running) break;

        std::string err;
        if (!runFullPipeline(snapshot, g_raw_mode, req.out_path, err)) {
            if (!g_running && err == kShutdownAbortReason) {
                std::cout << "[camera] capture dropped during shutdown: req_id=" << req.req_id
                          << ", object_id=" << req.object_id << std::endl;
                break;
            }
            std::cerr << "[camera] capture failed: req_id=" << req.req_id
                      << ", object_id=" << req.object_id
                      << ", reason=" << (err.empty() ? "CAPTURE_FAIL" : err) << std::endl;
            continue;
        }

        std::cout << "[camera] capture saved: req_id=" << req.req_id
                  << ", object_id=" << req.object_id
                  << ", out=" << req.out_path << std::endl;
    }
}

void triggerListenerThread() {
    ::unlink(kTriggerSocketPath);

    const int listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "[camera] trigger socket create failed" << std::endl;
        requestShutdown("trigger_socket_create_failed");
        return;
    }
    g_listener_fd.store(listen_fd);

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kTriggerSocketPath, sizeof(addr.sun_path) - 1);

    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[camera] trigger socket bind failed" << std::endl;
        close_fd_slot(g_listener_fd);
        requestShutdown("trigger_socket_bind_failed");
        return;
    }
    if (::listen(listen_fd, 16) < 0) {
        std::cerr << "[camera] trigger socket listen failed" << std::endl;
        close_fd_slot(g_listener_fd);
        requestShutdown("trigger_socket_listen_failed");
        return;
    }

    std::cout << "[camera] trigger listener ready: " << kTriggerSocketPath << std::endl;

    pollfd listen_pfd {};
    listen_pfd.fd = listen_fd;
    listen_pfd.events = POLLIN;

    while (g_running) {
        listen_pfd.revents = 0;
        const int poll_ret = ::poll(&listen_pfd, 1, 200);
        if (poll_ret < 0) {
            if (!g_running) break;
            if (errno == EINTR) continue;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (poll_ret == 0) continue;
        if ((listen_pfd.revents & POLLIN) == 0) {
            if (!g_running) break;
            if ((listen_pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;
        }

        const int client_fd = ::accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (!g_running) break;
            if (errno == EINTR) continue;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        g_active_client_fd.store(client_fd);

        if (!g_running) {
            std::cerr << "[camera] capture request dropped: reason=SHUTDOWN" << std::endl;
            close_fd_slot(g_active_client_fd);
            break;
        }

        std::string line;
        if (!read_line_with_timeout(client_fd, kClientReadTimeoutMs, line)) {
            std::cerr << "[camera] capture request dropped: reason="
                      << (g_running ? "READ_TIMEOUT" : kShutdownAbortReason) << std::endl;
            close_fd_slot(g_active_client_fd);
            if (!g_running) break;
            continue;
        }

        if (!g_running) {
            std::cerr << "[camera] capture request dropped: reason=SHUTDOWN" << std::endl;
            close_fd_slot(g_active_client_fd);
            break;
        }

        if (line.rfind("CAPTURE_REQ|", 0) != 0) {
            std::cerr << "[camera] capture request dropped: reason=INVALID_PREFIX" << std::endl;
            close_fd_slot(g_active_client_fd);
            continue;
        }

        const std::map<std::string, std::string> kv = parse_kv_line(line);
        const auto req_it = kv.find("REQ_ID");
        const auto obj_it = kv.find("OBJECT_ID");
        const auto tag_it = kv.find("TAG");
        const auto out_it = kv.find("OUT");
        if (req_it == kv.end() || obj_it == kv.end() || tag_it == kv.end() || out_it == kv.end()) {
            std::cerr << "[camera] capture request dropped: reason=MISSING_FIELD" << std::endl;
            close_fd_slot(g_active_client_fd);
            continue;
        }

        const std::string req_id = req_it->second;
        if (!is_safe_pending_output(out_it->second)) {
            std::cerr << "[camera] capture request dropped: req_id=" << req_id
                      << ", reason=OUT_PATH_NOT_ALLOWED, out=" << out_it->second << std::endl;
            close_fd_slot(g_active_client_fd);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(g_request_mutex);
            if (!g_running) {
                std::cerr << "[camera] capture request dropped: req_id=" << req_id
                          << ", reason=SHUTDOWN" << std::endl;
                close_fd_slot(g_active_client_fd);
                break;
            }
            if (static_cast<int>(g_request_queue.size()) >= kRequestQueueMax) {
                std::cerr << "[camera] capture request dropped: req_id=" << req_id
                          << ", reason=QUEUE_FULL" << std::endl;
                close_fd_slot(g_active_client_fd);
                continue;
            }

            CaptureRequest req;
            req.req_id = req_id;
            req.object_id = obj_it->second;
            req.tag = tag_it->second;
            req.out_path = out_it->second;
            g_request_queue.push(std::move(req));
        }
        close_fd_slot(g_active_client_fd);
        g_request_cv.notify_one();
    }

    close_fd_slot(g_active_client_fd);
    close_fd_slot(g_listener_fd);
    ::unlink(kTriggerSocketPath);
}

// ── 셔터 속도 설정 ──────────────────────────────────────────
// 역광 환경에서 픽셀 포화를 억제하기 위해 노출 시간을 제한합니다.
// kExposureTimeUs: 마이크로초 단위 (기본 8000 µs = 8 ms)
//   ↓ 값을 낮출수록 셔터가 빨라져 밝은 영역의 포화를 방지
//   ↑ 값을 높이면 어두운 환경에서 밝기 확보
// kAnalogueGain: 센서 아날로그 게인 (기본 1.0, 셔터를 줄인 만큼 보상)
static const std::string PIPE_BGR =
    "libcamerasrc ! "
    "video/x-raw,width=1920,height=1080,framerate=30/1 ! "
    "videoconvert ! video/x-raw,format=BGR ! "
    "appsink drop=true max-buffers=1 emit-signals=false wait-on-eos=false";

struct PipelineCandidate {
    const char* name;
    const char* pipeline;
    bool raw_mode;
};

const std::vector<PipelineCandidate> kPipelineCandidates = {
    {
        "RAW Bayer (stream-role=raw, bggr16le)",
        "libcamerasrc name=src src::stream-role=raw ! "
        "video/x-bayer,format=bggr16le,width=1920,height=1080,framerate=30/1 ! "
        "appsink drop=true max-buffers=1 emit-signals=false wait-on-eos=false",
        true,
    },
    {
        "RAW Bayer (default role, bggr16le)",
        "libcamerasrc ! "
        "video/x-bayer,format=bggr16le,width=1920,height=1080,framerate=30/1 ! "
        "appsink drop=true max-buffers=1 emit-signals=false wait-on-eos=false",
        true,
    },
    {
        "RAW Bayer (SBGGR10)",
        "libcamerasrc ! "
        "video/x-raw,format=SBGGR10,width=1920,height=1080,framerate=30/1 ! "
        "appsink drop=true max-buffers=1 emit-signals=false wait-on-eos=false",
        true,
    },
    {
        "BGR fallback",
        PIPE_BGR.c_str(),
        false,
    },
};

bool openBestPipeline(cv::VideoCapture& cap) {
    for (const PipelineCandidate& candidate : kPipelineCandidates) {
        std::cout << "[camera] opening pipeline: " << candidate.name << std::endl;
        if (!cap.open(candidate.pipeline, cv::CAP_GSTREAMER)) {
            std::cerr << "[camera] pipeline open failed: " << candidate.name << std::endl;
            continue;
        }

        g_raw_mode = candidate.raw_mode;
        g_active_pipeline_name = candidate.name;
        std::cout << "[camera] pipeline enabled: " << candidate.name << std::endl;
        return true;
    }

    g_raw_mode = false;
    g_active_pipeline_name.clear();
    return false;
}

bool waitForRawHelperReady(pid_t pid, int timeout_ms) {
    const std::uint64_t deadline = monotonic_ms_now() + timeout_ms;
    while (monotonic_ms_now() < deadline) {
        int status = 0;
        const pid_t wait_rc = ::waitpid(pid, &status, WNOHANG);
        if (wait_rc == pid) return false;

        if (::access(kRawHelperSocketPath, F_OK) == 0) {
            const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd >= 0) {
                sockaddr_un addr {};
                addr.sun_family = AF_UNIX;
                std::strncpy(addr.sun_path, kRawHelperSocketPath, sizeof(addr.sun_path) - 1);
                const int conn_rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
                if (conn_rc == 0) {
                    const std::string ping = "PING\n";
                    if (::write(fd, ping.c_str(), ping.size()) >= 0) {
                        std::string response;
                        if (read_line_with_timeout(fd, 1000, response) && response == "OK") {
                            ::close(fd);
                            return true;
                        }
                    }
                }
                ::close(fd);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

void stopNativeRawHelper() {
    if (g_raw_helper_pid > 0) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            sockaddr_un addr {};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, kRawHelperSocketPath, sizeof(addr.sun_path) - 1);
            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                const std::string stop_cmd = "STOP\n";
                ::write(fd, stop_cmd.c_str(), stop_cmd.size());
                std::string response;
                read_line_with_timeout(fd, 1000, response);
            }
            ::close(fd);
        }

        ::kill(g_raw_helper_pid, SIGTERM);
        ::waitpid(g_raw_helper_pid, nullptr, 0);
        g_raw_helper_pid = -1;
    }
    ::unlink(kRawHelperSocketPath);
}

bool enableNativeRawHelper() {
    constexpr const char* kHelperPath =
        "/home/iam/SFEPS/Camera/image_processing/scripts/capture_raw_frame.py";
    std::ifstream ifs(kHelperPath);
    if (!ifs.good()) return false;

    stopNativeRawHelper();

    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        ::execlp("python3", "python3",
                 kHelperPath,
                 "--server",
                 "--socket-path", kRawHelperSocketPath,
                 "--width", "1920",
                 "--height", "1080",
                 "--warmup-ms", "250",
                 "--shutter-us", "33000",
                 "--gain", "1.0",
                 static_cast<char*>(nullptr));
        std::_Exit(127);
    }

    if (!waitForRawHelperReady(pid, 5000)) {
        ::kill(pid, SIGTERM);
        ::waitpid(pid, nullptr, 0);
        ::unlink(kRawHelperSocketPath);
        return false;
    }

    g_raw_helper_pid = pid;
    g_use_native_raw_helper = true;
    g_raw_mode = true;
    g_active_pipeline_name = "Picamera2 RAW helper";
    std::cout << "[camera] pipeline enabled: " << g_active_pipeline_name << std::endl;
    return true;
}

}  // namespace

int main() {
    sigset_t signal_set {};
    if (!block_termination_signals(signal_set)) {
        return -1;
    }
    signal(SIGPIPE, SIG_IGN);

    std::thread signal_thread(signalWaitThread, signal_set);

#if LIVE_CAMERA_MODE
    // 파이프라인 열기 전에 libcamera 노출 튜닝 파일 생성 (역광 포화 억제)
    cv::VideoCapture cap;
    std::thread capture_thread;
    if (!enableNativeRawHelper()) {
        if (!openBestPipeline(cap)) {
            std::cerr << "[camera] camera pipeline open failed" << std::endl;
            stopSignalThread(signal_thread);
            return -1;
        }
        g_capture_device.store(&cap);
        capture_thread = std::thread(captureThreadFunc, std::ref(cap));
    }

    std::thread listener_thread(triggerListenerThread);
    std::thread worker_thread(pipelineWorkerThread);

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    release_capture_device();
    if (capture_thread.joinable()) capture_thread.join();
    if (listener_thread.joinable()) listener_thread.join();
    if (worker_thread.joinable()) worker_thread.join();
    g_capture_device.store(nullptr);
    cap.release();
#else
    cv::Mat frame = cv::imread("img/test_image.jpg", cv::IMREAD_UNCHANGED);
    if (frame.empty()) {
        stopSignalThread(signal_thread);
        return -1;
    }
    std::string err;
    if (!runFullPipeline(frame, frame.type() == CV_16UC1, "4_best_shot_local.jpg", err)) {
        stopSignalThread(signal_thread);
        return -1;
    }
#endif

    stopSignalThread(signal_thread);
    if (g_shutdown_started_ms.load() != 0) {
        const std::uint64_t elapsed_ms = monotonic_ms_now() - g_shutdown_started_ms.load();
        std::cout << "[camera] shutdown complete: elapsed_ms=" << elapsed_ms << std::endl;
    }
    return 0;
}
