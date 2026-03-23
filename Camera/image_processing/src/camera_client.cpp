// camera_client.cpp
// Server-triggered capture worker:
// - capture thread keeps latest frame
// - trigger listener receives CAPTURE_REQ over local UDS
// - pipeline worker writes requested output path and replies CAPTURE_ACK

#include <opencv2/opencv.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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
constexpr int kRequestQueueMax = 5;
constexpr int kClientReadTimeoutMs = 1000;

std::atomic<bool> g_running {true};
std::atomic<int> g_listener_fd {-1};
bool g_raw_mode = false;

std::mutex g_raw_mutex;
std::queue<cv::Mat> g_raw_queue;

struct CaptureRequest {
    int client_fd = -1;
    std::string req_id;
    std::string object_id;
    std::string tag;
    std::string out_path;
};

std::mutex g_request_mutex;
std::condition_variable g_request_cv;
std::queue<CaptureRequest> g_request_queue;

std::string sanitize_field(std::string value) {
    for (char& c : value) {
        if (c == '|' || c == '\n' || c == '\r') c = '_';
    }
    return value;
}

bool send_all(int fd, const std::string& msg) {
    std::size_t sent = 0;
    while (sent < msg.size()) {
        const ssize_t n = ::write(fd, msg.data() + sent, msg.size() - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

std::string now_utc_iso8601() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t tt = system_clock::to_time_t(now);
    std::tm tm_utc {};
    gmtime_r(&tt, &tm_utc);

    char buf[64];
    std::snprintf(buf,
                  sizeof(buf),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                  tm_utc.tm_year + 1900,
                  tm_utc.tm_mon + 1,
                  tm_utc.tm_mday,
                  tm_utc.tm_hour,
                  tm_utc.tm_min,
                  tm_utc.tm_sec,
                  static_cast<long long>(ms.count()));
    return std::string(buf);
}

void send_ack_ok(int fd, const CaptureRequest& req, const std::string& path) {
    const std::string msg =
        "CAPTURE_ACK|REQ_ID=" + sanitize_field(req.req_id) +
        "|OK=1|PATH=" + sanitize_field(path) +
        "|TS=" + sanitize_field(now_utc_iso8601()) + "\n";
    send_all(fd, msg);
}

void send_ack_fail(int fd, const std::string& req_id, const std::string& err) {
    const std::string msg =
        "CAPTURE_ACK|REQ_ID=" + sanitize_field(req_id) +
        "|OK=0|ERR=" + sanitize_field(err) +
        "|TS=" + sanitize_field(now_utc_iso8601()) + "\n";
    send_all(fd, msg);
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

void signalHandler(int) {
    g_running = false;
    g_request_cv.notify_all();
    const int fd = g_listener_fd.load();
    if (fd >= 0) ::close(fd);
}

#if LIVE_CAMERA_MODE
void captureThreadFunc(cv::VideoCapture& cap) {
    while (g_running) {
        if (!cap.grab()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        cv::Mat frame;
        cap.retrieve(frame);
        if (frame.empty()) continue;

        std::lock_guard<std::mutex> lock(g_raw_mutex);
        while (!g_raw_queue.empty()) g_raw_queue.pop();
        g_raw_queue.push(std::move(frame));
    }
}
#endif

bool runFullPipeline(const cv::Mat& frame,
                     bool raw_mode,
                     const std::string& out_path,
                     std::string& err) {
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

    cv::Mat tuning_view;
    cv::Mat best_frame = processISPAndGetBest(isp_out, tuning_view);
    cv::imwrite("3_tuning_viewer.jpg", tuning_view);

    const std::string out_dir = parent_dir_of(out_path);
    if (out_dir.empty() || !ensure_dir_exists(out_dir)) {
        err = "mkdir failed";
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
            if (!g_running && g_request_queue.empty()) break;
            req = std::move(g_request_queue.front());
            g_request_queue.pop();
        }

        cv::Mat snapshot;
        {
            std::lock_guard<std::mutex> lock(g_raw_mutex);
            if (!g_raw_queue.empty()) snapshot = g_raw_queue.front().clone();
        }

        if (snapshot.empty()) {
            send_ack_fail(req.client_fd, req.req_id, "NO_FRAME");
            ::close(req.client_fd);
            continue;
        }

        std::string err;
        if (!runFullPipeline(snapshot, g_raw_mode, req.out_path, err)) {
            send_ack_fail(req.client_fd, req.req_id, err.empty() ? "CAPTURE_FAIL" : err);
            ::close(req.client_fd);
            continue;
        }

        send_ack_ok(req.client_fd, req, req.out_path);
        ::close(req.client_fd);
    }
}

void triggerListenerThread() {
    ::unlink(kTriggerSocketPath);

    const int listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "[camera] trigger socket create failed" << std::endl;
        g_running = false;
        return;
    }
    g_listener_fd.store(listen_fd);

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kTriggerSocketPath, sizeof(addr.sun_path) - 1);

    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[camera] trigger socket bind failed" << std::endl;
        ::close(listen_fd);
        g_listener_fd.store(-1);
        g_running = false;
        return;
    }
    if (::listen(listen_fd, 16) < 0) {
        std::cerr << "[camera] trigger socket listen failed" << std::endl;
        ::close(listen_fd);
        g_listener_fd.store(-1);
        g_running = false;
        return;
    }

    std::cout << "[camera] trigger listener ready: " << kTriggerSocketPath << std::endl;

    while (g_running) {
        const int client_fd = ::accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (!g_running) break;
            if (errno == EINTR) continue;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        std::string line;
        if (!read_line_with_timeout(client_fd, kClientReadTimeoutMs, line)) {
            send_ack_fail(client_fd, "unknown", "READ_TIMEOUT");
            ::close(client_fd);
            continue;
        }

        if (line.rfind("CAPTURE_REQ|", 0) != 0) {
            send_ack_fail(client_fd, "unknown", "INVALID_PREFIX");
            ::close(client_fd);
            continue;
        }

        const std::map<std::string, std::string> kv = parse_kv_line(line);
        const auto req_it = kv.find("REQ_ID");
        const auto obj_it = kv.find("OBJECT_ID");
        const auto tag_it = kv.find("TAG");
        const auto out_it = kv.find("OUT");
        if (req_it == kv.end() || obj_it == kv.end() || tag_it == kv.end() || out_it == kv.end()) {
            send_ack_fail(client_fd, req_it == kv.end() ? "unknown" : req_it->second, "MISSING_FIELD");
            ::close(client_fd);
            continue;
        }

        const std::string req_id = req_it->second;
        if (!is_safe_pending_output(out_it->second)) {
            send_ack_fail(client_fd, req_id, "OUT_PATH_NOT_ALLOWED");
            ::close(client_fd);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(g_request_mutex);
            if (static_cast<int>(g_request_queue.size()) >= kRequestQueueMax) {
                send_ack_fail(client_fd, req_id, "QUEUE_FULL");
                ::close(client_fd);
                continue;
            }

            CaptureRequest req;
            req.client_fd = client_fd;
            req.req_id = req_id;
            req.object_id = obj_it->second;
            req.tag = tag_it->second;
            req.out_path = out_it->second;
            g_request_queue.push(std::move(req));
        }
        g_request_cv.notify_one();
    }

    ::close(listen_fd);
    g_listener_fd.store(-1);
    ::unlink(kTriggerSocketPath);
}

static const std::string PIPE_RAW =
    "libcamerasrc ! "
    "video/x-raw,format=SRGGB10,width=1920,height=1080,framerate=30/1 ! "
    "appsink drop=true max-buffers=1 emit-signals=false wait-on-eos=false";

static const std::string PIPE_BGR =
    "libcamerasrc ! "
    "video/x-raw,width=1920,height=1080,framerate=30/1 ! "
    "videoconvert ! video/x-raw,format=BGR ! "
    "appsink drop=true max-buffers=1 emit-signals=false wait-on-eos=false";

}  // namespace

int main() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGPIPE, SIG_IGN);

#if LIVE_CAMERA_MODE
    cv::VideoCapture cap(PIPE_RAW, cv::CAP_GSTREAMER);
    if (cap.isOpened()) {
        g_raw_mode = true;
        std::cout << "[camera] RAW pipeline enabled" << std::endl;
    } else {
        cap.open(PIPE_BGR, cv::CAP_GSTREAMER);
        if (!cap.isOpened()) {
            std::cerr << "[camera] camera pipeline open failed" << std::endl;
            return -1;
        }
        g_raw_mode = false;
        std::cout << "[camera] BGR fallback enabled" << std::endl;
    }

    std::thread capture_thread(captureThreadFunc, std::ref(cap));
    std::thread listener_thread(triggerListenerThread);
    std::thread worker_thread(pipelineWorkerThread);

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (capture_thread.joinable()) capture_thread.join();
    if (listener_thread.joinable()) listener_thread.join();
    if (worker_thread.joinable()) worker_thread.join();
    cap.release();
#else
    cv::Mat frame = cv::imread("img/test_image4.jpg", cv::IMREAD_UNCHANGED);
    if (frame.empty()) return -1;
    std::string err;
    if (!runFullPipeline(frame, frame.type() == CV_16UC1, "4_best_shot_local.jpg", err)) {
        return -1;
    }
#endif
    return 0;
}
