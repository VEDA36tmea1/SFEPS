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
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../inc/img_processing.h"

#define LIVE_CAMERA_MODE 1

namespace {

constexpr const char* kTriggerSocketPath = "/tmp/sfeps_camera_trigger.sock";
constexpr const char* kPendingBaseDir = "/home/iam/SFEPS/event_images/pending";
constexpr const char* kShutdownAbortReason = "SHUTDOWN";
constexpr int kRequestQueueMax = 5;
constexpr int kClientReadTimeoutMs = 1000;

std::atomic<bool> g_running {true};
std::atomic<bool> g_shutdown_started {false};
std::atomic<bool> g_signal_thread_stop {false};
std::atomic<int> g_listener_fd {-1};
std::atomic<int> g_active_client_fd {-1};
std::atomic<std::uint64_t> g_shutdown_started_ms {0};
std::atomic<cv::VideoCapture*> g_capture_device {nullptr};
bool g_raw_mode = false;

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
    g_request_cv.notify_all();
    release_capture_device();
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

bool runFullPipeline(const cv::Mat& frame,
                     bool raw_mode,
                     const std::string& out_path,
                     std::string& err) {
    if (!g_running) {
        err = kShutdownAbortReason;
        return false;
    }

    if (frame.empty()) {
        err = "empty frame";
        return false;
    }

    cv::imwrite("1_raw_capture.jpg", frame);

    cv::Mat isp_out;
    if (raw_mode) {
        if (frame.type() != CV_16UC1) {
            isp_out = frame;
        } else {
            isp_out = runPureISP(frame);
            cv::imwrite("2_pure_isp_out.jpg", isp_out);
        }
    } else {
        isp_out = frame;
    }

    if (!g_running) {
        err = kShutdownAbortReason;
        return false;
    }

    cv::Mat tuning_view;
    cv::Mat best_frame = processISPAndGetBest(isp_out, tuning_view);
    cv::imwrite("3_tuning_viewer.jpg", tuning_view);

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

    if (!cv::imwrite(out_path, best_frame)) {
        err = "cv::imwrite failed";
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
        {
            std::lock_guard<std::mutex> lock(g_raw_mutex);
            if (!g_raw_queue.empty()) snapshot = g_raw_queue.front().clone();
        }

        if (!g_running) break;

        if (snapshot.empty()) {
            std::cerr << "[camera] capture failed: req_id=" << req.req_id
                      << ", object_id=" << req.object_id << ", reason=NO_FRAME" << std::endl;
            continue;
        }

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
constexpr int    kExposureTimeUs  = 8000;   // 8 ms — 역광 포화 억제용
constexpr float  kAnalogueGain    = 1.0f;   // 센서 아날로그 게인

static const std::string PIPE_RAW =
    "libcamerasrc ! "
    "video/x-raw,format=SBGGR10,width=1920,height=1080,framerate=30/1 ! "
    "appsink drop=true max-buffers=1 emit-signals=false wait-on-eos=false";

static const std::string PIPE_BGR =
    "libcamerasrc ! "
    "video/x-raw,width=1920,height=1080,framerate=30/1 ! "
    "videoconvert ! video/x-raw,format=BGR ! "
    "appsink drop=true max-buffers=1 emit-signals=false wait-on-eos=false";

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

    cv::VideoCapture cap(PIPE_RAW, cv::CAP_GSTREAMER);
    if (cap.isOpened()) {
        g_raw_mode = true;
        std::cout << "[camera] RAW pipeline enabled" << std::endl;
    } else {
        cap.open(PIPE_BGR, cv::CAP_GSTREAMER);
        if (!cap.isOpened()) {
            std::cerr << "[camera] camera pipeline open failed" << std::endl;
            stopSignalThread(signal_thread);
            return -1;
        }
        g_raw_mode = false;
        std::cout << "[camera] BGR fallback enabled" << std::endl;
    }

    g_capture_device.store(&cap);
    std::thread capture_thread(captureThreadFunc, std::ref(cap));
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
