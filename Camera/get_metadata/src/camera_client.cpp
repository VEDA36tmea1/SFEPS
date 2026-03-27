
// camera_client.cpp
// - RTSPClient 로 ONVIF 메타데이터(XML) 수신
// - XMLParser 로 Human 바운딩 박스 파싱
// - OpenCV 로 영상 표시 + 박스 그리기
// - 마우스 클릭 시, 선택된 사람 바운딩 박스의 bottom-Y 기준 좌표를 stdout 으로 출력
// - (선택) MediaPipe Pose: 사람 bbox(가장 큰 것) ROI를 잘라 키포인트를 overlay
//   - 기본 on, --pose-off 로 끌 수 있음
//   - 디버그 출력: --pose-print
//
// 사용 예 (단독 실행/테스트):
//   g++ camera_client.cpp RTSPClient.cpp XMLParser.cpp -lopencv_core -lopencv_highgui -lopencv_imgproc -std=c++17 ...
//   ./camera_client
//
// 이후에는 stdout 을 ubuntu_tcp_server 입력 파이프로 연결해서
// bottom-Y 좌표가 서버로 전달되도록 응용 가능.

#include "RTSPClient.h"
#include "XMLParser.h"
#include "Config.h"
#include "re_id.h"

#include <opencv2/opencv.hpp>

#include <filesystem>
#include <array>
#include <sstream>
#include <utility>
#ifndef _WIN32
#include <poll.h>
#else
#include <windows.h>
#include <errno.h>
#endif

// POSIX 헤더들은 Windows(MSVC)에서 사용할 수 없어서 가드 처리한다.
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

#include <atomic>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <chrono>

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_detect_all{false};  // true 이면 Human 이외 타입도 모두 박스로 표시

static std::atomic<bool> g_pose_enabled{true};  // mediapipe worker 호출 여부
static constexpr int POSE_EVERY_N_FRAMES = 5;
static constexpr int POSE_LANDMARKS = 33;
static constexpr int POSE_RESPONSE_TIMEOUT_MS = 2000;

static bool g_pose_print = false; // 디버그용: 키포인트 콘솔 출력
static std::atomic<bool> g_debug_ratio{false}; // 'd' 토글: 선택 객체 내부 클릭 비율 출력

static std::mutex g_obj_mutex;
static std::vector<ParsedMetadataObject> g_objects;  // 마지막 프레임 기준 Human 객체들

// ── IdStabilizer ─────────────────────────────────────────────────────
static IdStabilizer g_stabilizer;
static std::mutex g_stable_mutex;
static std::vector<StableObject> g_stable_objects;

static cv::Mat g_last_frame;
static std::mutex g_frame_mutex;

// 선택된 객체(클릭으로 선택)
static std::mutex g_sel_mutex;
static std::string g_selected_id;
static cv::Rect g_selected_rect;
static bool g_selected_valid = false;

static void configure_low_latency_capture()
{
#ifdef _WIN32
    // OpenCV FFmpeg backend option string: key;value|key;value
    _putenv_s("OPENCV_FFMPEG_CAPTURE_OPTIONS",
              "rtsp_transport;tcp|fflags;nobuffer|flags;low_delay|max_delay;0");
#else
    setenv("OPENCV_FFMPEG_CAPTURE_OPTIONS",
           "rtsp_transport;tcp|fflags;nobuffer|flags;low_delay|max_delay;0", 1);
#endif
}

static void signal_handler(int) {
    g_running = false;
}

// 메타데이터 수신 + XML 파싱 스레드
static void metadata_thread_fn(RTSPClient* client, XMLParser* parser)
{
    unsigned char header[4];
    char* big_buffer = new char[65536];
    std::string accumulated_xml;
    unsigned int last_timestamp = 0;
    socket_t sock = client->getSocket();

    while (g_running) {
        client->sendHeartbeat();

        int read_len = recv(sock, reinterpret_cast<char*>(header), 4, MSG_WAITALL);
        if (read_len <= 0) break;
        if (header[0] != '$') continue; // interleaved RTP 가 아니면 무시

        int channel = (int)header[1];
        int payload_len = ((int)header[2] << 8) | (int)header[3];

        int total_read = 0;
        while (total_read < payload_len) {
            int to_read = payload_len - total_read;
            if (to_read > 65536) to_read = 65536;
            int r = recv(sock, big_buffer + total_read, to_read, 0);
            if (r <= 0) {
                total_read = 0;
                break;
            }
            total_read += r;
        }
        if (total_read <= 12) continue;

        if (channel == 2) { // 메타데이터 채널 (RTSPClient main.cpp 와 동일 가정)
            unsigned char* rtp_ptr = (unsigned char*)big_buffer;
            unsigned int current_timestamp =
                (rtp_ptr[4] << 24) | (rtp_ptr[5] << 16) | (rtp_ptr[6] << 8) | rtp_ptr[7];
            char* xml_data = big_buffer + 12;
            int xml_len = total_read - 12;

            if (current_timestamp != last_timestamp && last_timestamp != 0) {
                // 한 프레임 분량의 XML 누적 완료 → 객체 파싱
                std::vector<ParsedMetadataObject> humans =
                    parser->parseHumanObjectsForAnalytics(accumulated_xml, g_detect_all.load());
                {
                    std::lock_guard<std::mutex> lock(g_obj_mutex);
                    g_objects = humans;  // copy (아래에서 사용)
                }

                // ── IdStabilizer 적용 ──
                {
                    std::vector<DetectedInput> inputs;
                    inputs.reserve(humans.size());
                    for (const auto& obj : humans) {
                        DetectedInput d;
                        d.camera_id = obj.id;
                        d.left = obj.left;  d.top = obj.top;
                        d.right = obj.right; d.bottom = obj.bottom;
                        d.cx = obj.x;  d.cy = obj.y;
                        inputs.push_back(d);
                    }
                    int64_t ts_ms = static_cast<int64_t>(current_timestamp / 90);
                    auto stable = g_stabilizer.update(inputs, ts_ms);
                    {
                        std::lock_guard<std::mutex> lock(g_stable_mutex);
                        g_stable_objects = std::move(stable);
                    }
                }

                accumulated_xml.clear();
            }
            accumulated_xml.append(xml_data, xml_len);
            last_timestamp = current_timestamp;
        }
    }

    delete[] big_buffer;
}

struct PoseResult
{
    bool ok{false};
    std::array<float, POSE_LANDMARKS * 3> lms_xyzv{}; // x,y,visibility each; normalized [0,1] in ROI space
    double encode_ms{0.0};
    double roundtrip_ms{0.0};
    double worker_ms{0.0};
    double fps_est{0.0};
};

struct PoseJob
{
    cv::Mat roi_bgr;
    cv::Rect roi_rect;
    std::chrono::steady_clock::time_point t_submit;
    bool valid{false};
};

static std::mutex g_pose_job_mutex;
static std::condition_variable g_pose_job_cv;
static PoseJob g_pose_job;
static std::atomic<bool> g_pose_thread_running{false};
static std::atomic<bool> g_pose_have_result{false};
static std::mutex g_pose_res_mutex;
static PoseResult g_pose_last_result;
static cv::Rect g_pose_last_roi;
static std::chrono::steady_clock::time_point g_pose_last_ok_time;
static double g_pose_fps_ema = 0.0;
static std::chrono::steady_clock::time_point g_pose_prev_ok_time;

static bool read_line_fd_timeout(int fd, std::string& out, int timeout_ms)
{
    out.clear();
    char c{};

#ifdef _WIN32
    // Windows에서는 PoseWorker를 스텁 처리하여 pose 요청이 비활성화된다.
    // 여기 함수는 호출되지 않으므로 컴파일만 통과하도록 실패로 처리한다.
    (void)fd;
    (void)out;
    (void)timeout_ms;
    return false;
#else
    while (true)
    {
        pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = ::poll(&pfd, 1, timeout_ms);
        if (pr == 0)
            return false; // timeout
        if (pr < 0)
            return false;

        ssize_t r = ::read(fd, &c, 1);
        if (r <= 0)
            return false;
        if (c == '\n')
            break;
        out.push_back(c);
        if (out.size() > 100000)
            break; // safety
    }
    return true;
#endif
}

// Windows에서는 fork/pipe/waitpid 기반 pose worker를 안정적으로 돌리기 어려워 스텁 처리한다.
#ifdef _WIN32
struct PoseWorker
{
    bool active{false};
    bool start() { return false; }
    void stop() { active = false; }
    bool estimate(const cv::Mat&, PoseResult& res)
    {
        res.ok = false;
        return false;
    }
};
#else
struct PoseWorker
{
    pid_t pid{-1};
    int write_fd{-1}; // C++ -> Python stdin
    int read_fd{-1};  // Python stdout -> C++
    bool active{false};

    std::string get_exe_dir() const
    {
        char buf[4096];
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0)
            return "";
        buf[n] = '\0';
        std::filesystem::path p(buf);
        return p.parent_path().string(); // camera_client 실행 파일이 있는 폴더(get_metadata)
    }

    bool start()
    {
        if (active)
            return true;

        std::string root_dir = get_exe_dir();
        if (root_dir.empty())
            return false;

        // venv 위치 우선순위:
        // 1) Camera/get_metadata/.venv
        // 2) Camera/.venv (통합 venv)
        std::filesystem::path py_path = std::filesystem::path(root_dir) / ".venv" / "bin" / "python";
        if (!std::filesystem::exists(py_path))
            py_path = std::filesystem::path(root_dir).parent_path() / ".venv" / "bin" / "python";
        std::filesystem::path script_path = std::filesystem::path(root_dir) / "src" / "mediapipe_pose_worker.py";
        if (!std::filesystem::exists(py_path) || !std::filesystem::exists(script_path))
        {
            std::cerr << "[pose] mediapipe worker files missing. py=" << py_path << " script=" << script_path << "\n";
            return false;
        }

        int pipe_in[2];
        int pipe_out[2];
        if (::pipe(pipe_in) != 0)
            return false;
        if (::pipe(pipe_out) != 0)
            return false;

        pid = ::fork();
        if (pid == 0)
        {
            // child
            ::dup2(pipe_in[0], STDIN_FILENO);
            ::dup2(pipe_out[1], STDOUT_FILENO);
            // stderr가 stdout에 섞이면 프로토콜(0/1 라인)이 깨질 수 있으니 버린다.
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0)
            {
                ::dup2(devnull, STDERR_FILENO);
                ::close(devnull);
            }

            ::close(pipe_in[0]);
            ::close(pipe_in[1]);
            ::close(pipe_out[0]);
            ::close(pipe_out[1]);

            const char* py = py_path.c_str();
            const char* script = script_path.c_str();
            char* const argv[] = {const_cast<char*>(py), const_cast<char*>(script), nullptr};
            ::execv(py, argv);
            _exit(127);
        }

        // parent
        ::close(pipe_in[0]);
        ::close(pipe_out[1]);
        write_fd = pipe_in[1];
        read_fd = pipe_out[0];

        active = true;
        std::cerr << "[pose] worker started pid=" << pid << "\n";
        return true;
    }

    void stop()
    {
        active = false;
        if (write_fd >= 0)
        {
            ::close(write_fd);
            write_fd = -1;
        }
        if (read_fd >= 0)
        {
            ::close(read_fd);
            read_fd = -1;
        }
        if (pid > 0)
        {
            ::kill(pid, SIGTERM);
            ::waitpid(pid, nullptr, 0);
            pid = -1;
        }
    }

    bool estimate(const cv::Mat& roi, PoseResult& res)
    {
        res.ok = false;
        if (!active)
            return false;
        if (roi.empty())
            return false;

        auto t0 = std::chrono::steady_clock::now();
        std::vector<uchar> buf;
        std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, 80};
        if (!cv::imencode(".jpg", roi, buf, params))
            return false;
        auto t1 = std::chrono::steady_clock::now();
        res.encode_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        uint32_t n = static_cast<uint32_t>(buf.size());
        uint8_t hdr[4];
        hdr[0] = (uint8_t)(n & 0xFF);
        hdr[1] = (uint8_t)((n >> 8) & 0xFF);
        hdr[2] = (uint8_t)((n >> 16) & 0xFF);
        hdr[3] = (uint8_t)((n >> 24) & 0xFF);

        // write header + bytes
        ssize_t w1 = ::write(write_fd, hdr, 4);
        if (w1 != 4)
            return false;
        ssize_t total = 0;
        while (total < (ssize_t)n)
        {
            ssize_t w = ::write(write_fd, buf.data() + total, n - total);
            if (w <= 0)
                return false;
            total += w;
        }

        // read response line (0/1 로 시작하는 라인만 채택)
        std::string line;
        bool got = false;
        for (int tries = 0; tries < 3; ++tries)
        {
            if (!read_line_fd_timeout(read_fd, line, POSE_RESPONSE_TIMEOUT_MS))
                continue;
            if (!line.empty() && (line[0] == '0' || line[0] == '1'))
            {
                got = true;
                break;
            }
        }
        if (!got)
            return false;

        auto t2 = std::chrono::steady_clock::now();
        res.roundtrip_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();

        std::istringstream iss(line);
        int has = 0;
        if (!(iss >> has))
            return false;
        if (has == 0)
            return true; // ok=false

        res.ok = true;
        for (int i = 0; i < POSE_LANDMARKS; ++i)
        {
            float x, y, v;
            if (!(iss >> x >> y >> v))
                break;
            res.lms_xyzv[i * 3 + 0] = x;
            res.lms_xyzv[i * 3 + 1] = y;
            res.lms_xyzv[i * 3 + 2] = v;
        }
        return true;
    }
};

#endif // _WIN32 pose stub

static PoseWorker g_pose_worker;

static bool compute_rect_from_obj(const ParsedMetadataObject& obj, int W, int H, cv::Rect& out)
{
    int left, right, top, bottom;
    if (std::max({obj.left, obj.right, obj.top, obj.bottom}) <= 1.5f)
    {
        left = static_cast<int>(obj.left * W);
        right = static_cast<int>(obj.right * W);
        top = static_cast<int>(obj.top * H);
        bottom = static_cast<int>(obj.bottom * H);
    }
    else
    {
        const double sx = static_cast<double>(W) / SENSOR_WIDTH;
        const double sy = static_cast<double>(H) / SENSOR_HEIGHT;
        left = static_cast<int>(obj.left * sx);
        right = static_cast<int>(obj.right * sx);
        top = static_cast<int>(obj.top * sy);
        bottom = static_cast<int>(obj.bottom * sy);
    }

    int width = std::max(1, right - left);
    int height = std::max(1, bottom - top);
    out = cv::Rect(left, top, width, height);

    // clamp
    out.x = std::max(0, std::min(out.x, W - 1));
    out.y = std::max(0, std::min(out.y, H - 1));
    out.width = std::max(1, std::min(out.width, W - out.x));
    out.height = std::max(1, std::min(out.height, H - out.y));
    return (out.area() > 20);
}

static void pose_thread_fn()
{
    g_pose_thread_running = true;
    while (g_running)
    {
        PoseJob job;
        {
            std::unique_lock<std::mutex> lk(g_pose_job_mutex);
            g_pose_job_cv.wait_for(lk, std::chrono::milliseconds(50), [] { return g_pose_job.valid || !g_running.load(); });
            if (!g_running)
                break;
            if (!g_pose_job.valid)
                continue;
            job = g_pose_job;
            g_pose_job.valid = false; // take it
        }

        PoseResult res;
        auto t_start = std::chrono::steady_clock::now();
        bool ok = g_pose_worker.estimate(job.roi_bgr, res);
        auto t_end = std::chrono::steady_clock::now();
        res.worker_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

        if (ok && res.ok)
        {
            // FPS EMA (추정 성공한 경우 기준)
            auto now = t_end;
            if (g_pose_prev_ok_time.time_since_epoch().count() != 0)
            {
                double dt = std::chrono::duration<double>(now - g_pose_prev_ok_time).count();
                double inst_fps = (dt > 1e-6) ? (1.0 / dt) : 0.0;
                if (g_pose_fps_ema <= 0.0)
                    g_pose_fps_ema = inst_fps;
                else
                    g_pose_fps_ema = 0.8 * g_pose_fps_ema + 0.2 * inst_fps;
                res.fps_est = g_pose_fps_ema;
            }
            g_pose_prev_ok_time = now;
        }

        {
            std::lock_guard<std::mutex> lk(g_pose_res_mutex);
            g_pose_last_result = res;
            g_pose_last_roi = job.roi_rect;
            g_pose_have_result = true;
        }
    }
    g_pose_thread_running = false;
}

// 마우스 클릭 시, 해당 위치에 있는 Human 박스를 찾고 bottom-Y 픽셀 좌표를 stdout 으로 출력
static void on_mouse(int event, int x, int y, int /*flags*/, void* userdata)
{
    if (event != cv::EVENT_LBUTTONDOWN) return;

    cv::Mat* frame_ptr = static_cast<cv::Mat*>(userdata);
    cv::Mat frame_copy;
    {
        std::lock_guard<std::mutex> lock(g_frame_mutex);
        if (frame_ptr->empty()) return;
        frame_copy = frame_ptr->clone();
    }
    int W = frame_copy.cols;
    int H = frame_copy.rows;

    std::vector<ParsedMetadataObject> objs;
    {
        std::lock_guard<std::mutex> lock(g_obj_mutex);
        objs = g_objects;
    }

    for (const auto& obj : objs) {
        cv::Rect rect;
        if (!compute_rect_from_obj(obj, W, H, rect))
            continue;
        if (rect.contains(cv::Point(x, y))) {
            int bottom_y_px = rect.y + rect.height;
            int center_x_px = rect.x + rect.width / 2;

            {
                std::lock_guard<std::mutex> lock(g_sel_mutex);
                g_selected_id = obj.id;
                g_selected_rect = rect;
                g_selected_valid = true;
            }

            // 시각 피드백
            cv::rectangle(frame_copy, rect, cv::Scalar(0, 255, 0), 2);
            cv::circle(frame_copy, cv::Point(center_x_px, bottom_y_px), 6,
                       cv::Scalar(0, 0, 255), -1);
            cv::putText(frame_copy, ("SEL " + obj.id).c_str(), cv::Point(rect.x, rect.y - 10),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);
            cv::imshow("camera_client", frame_copy);

            // stdout 으로 결과 출력 (ubuntu_tcp_server 에 파이프로 연결해서 사용 가능)
            std::cout << "HUMAN_BOTTOM "
                      << "id=" << obj.id
                      << " type=" << obj.type
                      << " center_x_px=" << center_x_px
                      << " bottom_y_px=" << bottom_y_px
                      << std::endl;
            std::fflush(stdout);

            // 디버그: 선택 객체 내부 클릭 좌표의 정규화 비율 출력
            if (g_debug_ratio.load())
            {
                const float rx = (x - rect.x) / std::max(1.0f, (float)rect.width);
                const float ry = (y - rect.y) / std::max(1.0f, (float)rect.height);
                std::cout << "CLICK_RATIO "
                          << "id=" << obj.id
                          << " px=(" << x << "," << y << ")"
                          << " ratio=(" << rx << "," << ry << ")"
                          << std::endl;
                std::fflush(stdout);
            }
            break;
        }
    }
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return -1;
    }
#endif

    configure_low_latency_capture();

    // 인자: --detect-all 이면 Human 외 타입도 모두 표시
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--detect-all")
            g_detect_all = true;
        else if (arg == "--pose-off")
            g_pose_enabled = false;
        else if (arg == "--pose-print")
            g_pose_print = true;
    }

    std::signal(SIGINT, signal_handler);

    // 1. RTSPClient + XMLParser 준비
    RTSPClient client;
    XMLParser  parser;

    if (!client.connectToCamera()) {
        return -1;
    }
    client.sendHandshake();

    // 2. 메타데이터 수신 스레드 시작
    std::thread meta_thread(metadata_thread_fn, &client, &parser);

    // 3. 영상 스트림 (ONVIF 카메라 RTSP URL 사용)
    cv::VideoCapture cap(RTSP_URL, cv::CAP_FFMPEG);
    if (!cap.isOpened()) {
        std::cerr << "[camera_client] RTSP 영상 열기 실패: " << RTSP_URL << std::endl;
        g_running = false;
        meta_thread.join();
        return -1;
    }

    // Keep decoder queue short to reduce display lag.
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

    cv::namedWindow("camera_client", cv::WINDOW_NORMAL);
    cv::setMouseCallback("camera_client", on_mouse, &g_last_frame);

    PoseResult last_pose;
    cv::Rect last_pose_roi;
    last_pose.ok = false;

    bool pose_active = g_pose_enabled.load();
    if (pose_active)
    {
        if (!g_pose_worker.start())
        {
            pose_active = false;
            std::cerr << "[pose] worker start 실패 → pose overlay 비활성화\n";
        }
    }

    std::thread pose_thread;
    if (pose_active)
        pose_thread = std::thread(pose_thread_fn);

    int frame_counter = 0;
    while (g_running) {
        cv::Mat frame;

        // Drop a couple of queued frames each loop to stay close to live edge.
        int dropped = 0;
        while (dropped < 2 && cap.grab()) {
            ++dropped;
        }

        if (!cap.retrieve(frame) || frame.empty()) {
            std::cerr << "[camera_client] 빈 프레임, 종료" << std::endl;
            break;
        }

        {
            std::lock_guard<std::mutex> lock(g_frame_mutex);
            g_last_frame = frame.clone();
        }

        int W = frame.cols;
        int H = frame.rows;

        // 현재 Human bounding box 들을 그려준다 (안정 ID 사용)
        std::vector<StableObject> sobjs;
        {
            std::lock_guard<std::mutex> lock(g_stable_mutex);
            sobjs = g_stable_objects;
        }
        for (const auto& s : sobjs) {
            int left, right, top, bottom;
            if (std::max({std::fabs(s.left), std::fabs(s.right),
                          std::fabs(s.top),  std::fabs(s.bottom)}) <= 1.5f) {
                left   = static_cast<int>(s.left   * W);
                right  = static_cast<int>(s.right  * W);
                top    = static_cast<int>(s.top    * H);
                bottom = static_cast<int>(s.bottom * H);
            } else {
                const double sx = static_cast<double>(W) / SENSOR_WIDTH;
                const double sy = static_cast<double>(H) / SENSOR_HEIGHT;
                left   = static_cast<int>(s.left   * sx);
                right  = static_cast<int>(s.right  * sx);
                top    = static_cast<int>(s.top    * sy);
                bottom = static_cast<int>(s.bottom * sy);
            }
            int width  = std::max(1, right - left);
            int height = std::max(1, bottom - top);

            cv::Scalar color = s.is_recovered
                ? cv::Scalar(0, 165, 255)   // 주황 (ID 복구됨)
                : cv::Scalar(0, 255, 255);  // 노랑 (정상)

            cv::rectangle(frame, cv::Rect(left, top, width, height), color, 2);

            std::string label = s.stable_id;
            if (s.camera_id != s.stable_id)
                label += "(" + s.camera_id + ")";
            cv::putText(frame, label.c_str(), cv::Point(left, top - 5),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1);
        }

        // ── MediaPipe Pose 추정 (선택된 객체 bbox ROI 기준: 빠름) ──
        std::string sel_id;
        cv::Rect sel_rect;
        bool sel_ok = false;
        {
            std::lock_guard<std::mutex> lock(g_sel_mutex);
            sel_ok = g_selected_valid;
            sel_id = g_selected_id;
            sel_rect = g_selected_rect;
        }

        if (pose_active && sel_ok && (frame_counter % POSE_EVERY_N_FRAMES == 0))
        {
            // 선택된 rect는 클릭 순간 기준이라, frame 사이즈 clamp 다시 함
            sel_rect.x = std::max(0, std::min(sel_rect.x, W - 1));
            sel_rect.y = std::max(0, std::min(sel_rect.y, H - 1));
            sel_rect.width = std::max(1, std::min(sel_rect.width, W - sel_rect.x));
            sel_rect.height = std::max(1, std::min(sel_rect.height, H - sel_rect.y));

            // padding
            int pad_x = static_cast<int>(sel_rect.width * 0.1);
            int pad_y = static_cast<int>(sel_rect.height * 0.1);
            int x0 = std::max(0, sel_rect.x - pad_x);
            int y0 = std::max(0, sel_rect.y - pad_y);
            int x1 = std::min(W, sel_rect.x + sel_rect.width + pad_x);
            int y1 = std::min(H, sel_rect.y + sel_rect.height + pad_y);

            if (x1 > x0 && y1 > y0)
            {
                cv::Rect roi_rect(x0, y0, x1 - x0, y1 - y0);
                if (roi_rect.width > 10 && roi_rect.height > 10)
                {
                    // 비동기 job 제출 (이전 job은 덮어씀: 최신 프레임 우선)
                    PoseJob job;
                    job.roi_bgr = frame(roi_rect).clone();
                    job.roi_rect = roi_rect;
                    job.t_submit = std::chrono::steady_clock::now();
                    job.valid = true;
                    {
                        std::lock_guard<std::mutex> lk(g_pose_job_mutex);
                        g_pose_job = std::move(job);
                    }
                    g_pose_job_cv.notify_one();
                }
            }
        }

        // 선택 박스 강조 표시
        if (sel_ok)
        {
            cv::rectangle(frame, sel_rect, cv::Scalar(0, 255, 0), 2);
            cv::putText(frame, ("SEL " + sel_id).c_str(), cv::Point(sel_rect.x, sel_rect.y - 10),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);
        }

        // 비동기 결과 반영
        if (pose_active && g_pose_have_result.load())
        {
            std::lock_guard<std::mutex> lk(g_pose_res_mutex);
            last_pose = g_pose_last_result;
            last_pose_roi = g_pose_last_roi;
        }

        if (pose_active && last_pose.ok)
        {
            // landmark draw (visibility 임계값 적용)
            const float v_th = 0.4f;
            for (int i = 0; i < POSE_LANDMARKS; ++i)
            {
                float x = last_pose.lms_xyzv[i * 3 + 0];
                float y = last_pose.lms_xyzv[i * 3 + 1];
                float v = last_pose.lms_xyzv[i * 3 + 2];
                if (v < v_th)
                    continue;
                int px = last_pose_roi.x + static_cast<int>(x * last_pose_roi.width);
                int py = last_pose_roi.y + static_cast<int>(y * last_pose_roi.height);
                cv::circle(frame, cv::Point(px, py), 3, cv::Scalar(0, 0, 255), -1, cv::LINE_AA);
            }

            cv::putText(frame, "POSE OK", cv::Point(10, 20),
                        cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 0, 255), 2);

            // 성능 표시
            {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "pose fps~%.1f  enc=%.1fms  rt=%.1fms  total=%.1fms",
                              last_pose.fps_est, last_pose.encode_ms, last_pose.roundtrip_ms, last_pose.worker_ms);
                cv::putText(frame, buf, cv::Point(10, 45),
                            cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 255), 2);
            }

            if (g_pose_print)
            {
                // 몇 개 핵심 키포인트만 출력: nose(0), L/R shoulder(11/12), L/R hip(23/24), L/R ankle(27/28)
                auto kp = [&](int idx) -> std::pair<int, int> {
                    float x = last_pose.lms_xyzv[idx * 3 + 0];
                    float y = last_pose.lms_xyzv[idx * 3 + 1];
                    int px = last_pose_roi.x + static_cast<int>(x * last_pose_roi.width);
                    int py = last_pose_roi.y + static_cast<int>(y * last_pose_roi.height);
                    return {px, py};
                };
                auto n = kp(0);
                auto ls = kp(11);
                auto rs = kp(12);
                auto lh = kp(23);
                auto rh = kp(24);
                auto la = kp(27);
                auto ra = kp(28);
                std::cout << "POSE_KEYPX nose=(" << n.first << "," << n.second << ")"
                          << " Lsh=(" << ls.first << "," << ls.second << ")"
                          << " Rsh=(" << rs.first << "," << rs.second << ")"
                          << " Lhip=(" << lh.first << "," << lh.second << ")"
                          << " Rhip=(" << rh.first << "," << rh.second << ")"
                          << " Lank=(" << la.first << "," << la.second << ")"
                          << " Rank=(" << ra.first << "," << ra.second << ")"
                          << std::endl;
            }
        }
        else if (pose_active && sel_ok)
        {
            cv::putText(frame, "POSE ...", cv::Point(10, 20),
                        cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 0, 255), 2);
        }

        // ROI 시각화(디버그)
        if (pose_active && sel_ok && last_pose_roi.area() > 0)
        {
            cv::rectangle(frame, last_pose_roi, cv::Scalar(255, 0, 255), 2);
            cv::putText(frame, "ROI", cv::Point(last_pose_roi.x, std::max(0, last_pose_roi.y - 5)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(255, 0, 255), 2);
        }

        cv::imshow("camera_client", frame);
        int key = cv::waitKey(1) & 0xFF;
        if (key == 27 || key == 'q') {
            g_running = false;
            break;
        }
        if (key == 'd' || key == 'D') {
            bool cur = g_debug_ratio.load();
            g_debug_ratio = !cur;
            std::cerr << "[debug] click ratio " << (g_debug_ratio.load() ? "ON" : "OFF") << "\n";
        }
        frame_counter++;
    }

    g_running = false;
    if (meta_thread.joinable()) meta_thread.join();
    cap.release();
    cv::destroyAllWindows();
    if (pose_active)
        g_pose_worker.stop();
    if (pose_thread.joinable())
        pose_thread.join();

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}
