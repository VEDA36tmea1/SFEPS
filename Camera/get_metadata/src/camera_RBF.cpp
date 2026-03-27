// camera_RBF.cpp
// - camera_client.cpp 기반: ONVIF 메타데이터로 사람 bbox 표시/클릭 선택
// - 선택된 bbox에서 v = top + h * RATIO (기본 0.26) 지점을 "Z=1200mm 평면"으로 가정
// - (u,v) 픽셀을 RBF(Thin-Plate Spline)로 PWM(pan,tilt)으로 보간
// - stdout 으로 "SET_PWM,PAN=...,TILT=..." 출력 (ubuntu_tcp_server 파이프로 전달)
// - (옵션) 월드(X,Y, cm) -> 픽셀(u,v) RBF를 이용해 Z=1200mm 평면 그리드 오버레이 표시
// - DeepSORT Python 워커로 안정적 ID 부여 (별도 스레드)

#include "RTSPClient.h"
#include "XMLParser.h"
#include "Config.h"
#include "re_id.h"

#include <opencv2/opencv.hpp>
#include <opencv2/videoio/registry.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <cmath>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_detect_all{false};

// ── 캡처 전용 스레드 공유 변수 ──────────────────────────────────
// 별도 스레드가 RTSP 프레임을 계속 읽어두고, 메인 루프는 최신 프레임만 소비한다.
static std::mutex g_cap_mutex;
static std::condition_variable g_cap_cv;
static cv::Mat g_cap_latest_frame;
static uint64_t g_cap_seq = 0;
static std::chrono::steady_clock::time_point g_cap_last_ts = std::chrono::steady_clock::now();

// ── 파싱된 원시 bbox (카메라 ID 그대로) ─────────────────────────────
static std::mutex g_raw_obj_mutex;
static std::vector<ParsedMetadataObject> g_raw_objects;

// ── DeepSORT가 부여한 안정적 ID bbox ────────────────────────────────
static std::mutex g_obj_mutex;
static std::vector<ParsedMetadataObject> g_objects;

// ── IdStabilizer (DeepSORT 후단에서 ID 안정화) ──────────────────────
static IdStabilizer g_stabilizer;

// ── 안정화된 표시/인터랙션용 객체 버퍼 ──────────────────────────
// g_objects        = 트래커 내부 ID (NativeTrack "N1" 등)
// g_stable_objects = IdStabilizer 출력 stable_id — 렌더링·클릭에 사용
static std::mutex g_stable_mutex;
static std::vector<ParsedMetadataObject> g_stable_objects;

// DeepSORT 결과가 비는 순간에도(확정 전/일시 누락) 화면 bbox가 튀지 않게
// 최근 DeepSORT 결과를 짧게 유지한다.
static std::vector<ParsedMetadataObject> g_deepsort_last_objects;
static int g_deepsort_empty_count = 0;
static constexpr int DEEPSORT_EMPTY_GRACE_FRAMES = 5;

// ── ID별 bbox EMA 스무더 ─────────────────────────────────────────────
// DeepSORT 비동기 파이프라인의 프레임 지연 + to_ltrb() 칼만 예측값의
// 순간 점프를 EMA로 감쇠시켜 화면 bbox 떨림을 제거한다.
// BBOX_SMOOTH_ALPHA: 클수록 새 값에 빠르게 반응, 작을수록 부드러움
static constexpr double BBOX_SMOOTH_ALPHA = 0.35;
struct SmoothedRect { double x, y, w, h; bool init{false}; };
static std::map<std::string, SmoothedRect> g_bbox_smooth;

static cv::Mat g_last_frame;
static std::mutex g_frame_mutex;

static std::mutex g_sel_mutex;
static std::string g_selected_id;
static cv::Rect g_selected_rect;
static bool g_selected_valid = false;

static std::mutex g_pwm_mutex;
static int g_last_pan = 1500;
static int g_last_tilt = 1500;
static int g_last_target_u = -1;
static int g_last_target_v = -1;
static bool g_last_pwm_valid = false;

// QT 모드: 클릭 이벤트 무시, TRACK_START|id 수신 후에만 추적 시작
// 연결된 Qt 클라이언트 소켓으로 PWM_OUT 역방향 전송
static bool g_qt_mode = false;
#ifdef _WIN32
static SOCKET g_remote_client_fd = INVALID_SOCKET;
#else
static int g_remote_client_fd = -1;
#endif
static std::mutex g_remote_client_fd_mutex;

static std::mutex g_click_mutex;
static bool g_click_pending = false;
static std::string g_click_pending_id;
static std::mutex g_remote_msg_mutex;
static std::string g_remote_last_msg;
static std::string g_remote_last_id;
static std::string g_remote_src_host = "192.168.0.101";
static bool g_remote_tracking = false;
static std::chrono::steady_clock::time_point g_remote_last_rx_tp = std::chrono::steady_clock::now();
static bool g_remote_bbox_valid = false;
static double g_remote_l = 0.0, g_remote_t = 0.0, g_remote_r = 0.0, g_remote_b = 0.0; // normalized [0,1]

static void signal_handler(int) { g_running = false; }

// ── 캡처 환경 변수 설정 (FFmpeg low-latency RTSP) ───────────────
static void setup_low_latency_capture_env()
{
#ifdef _WIN32
    _putenv_s("OPENCV_FFMPEG_CAPTURE_OPTIONS",
        "rtsp_transport;tcp|fflags;nobuffer|flags;low_delay|max_delay;0"
        "|reorder_queue_size;0|analyzeduration;0|probesize;32768");
#else
    setenv("OPENCV_FFMPEG_CAPTURE_OPTIONS",
        "rtsp_transport;tcp|fflags;nobuffer|flags;low_delay|max_delay;0"
        "|reorder_queue_size;0|analyzeduration;0|probesize;32768", 1);
#endif
}

// ── RTSP URL 파싱 헬퍼 ───────────────────────────────────────────
static bool parse_rtsp_url(const std::string& url,
                            std::string& out_host, int& out_port, std::string& out_path)
{
    const std::string scheme = "rtsp://";
    if (url.rfind(scheme, 0) != 0) return false;
    std::string rest = url.substr(scheme.size());
    std::size_t at = rest.find('@');
    if (at != std::string::npos) rest = rest.substr(at + 1);
    std::size_t slash = rest.find('/');
    std::string host_port = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    out_path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    if (out_path.empty()) out_path = "/";
    if (host_port.empty()) return false;
    std::size_t colon = host_port.rfind(':');
    if (colon == std::string::npos)
    {
        out_host = host_port;
        out_port = 554;
        return !out_host.empty();
    }
    out_host = host_port.substr(0, colon);
    std::string port_str = host_port.substr(colon + 1);
    if (out_host.empty() || port_str.empty()) return false;
    try { out_port = std::stoi(port_str); } catch (...) { return false; }
    return (out_port > 0 && out_port <= 65535);
}

// ── GStreamer low-latency RTSP 파이프라인 구성 ───────────────────
static std::string build_gstreamer_rtsp_pipeline(bool use_tcp)
{
    std::string host; int port = 554; std::string path;
    if (!parse_rtsp_url(RTSP_URL, host, port, path)) return "";
    std::ostringstream oss;
    oss << "rtspsrc location=\"" << RTSP_URL << "\" "
        << "protocols=" << (use_tcp ? "tcp" : "udp") << " "
        << "latency=0 drop-on-latency=true ! "
        << "rtph264depay ! h264parse ! avdec_h264 ! "
        << "videoconvert ! appsink sync=false max-buffers=1 drop=true";
    return oss.str();
}

// ── 캡처 전용 스레드: RTSP 프레임을 계속 읽어 g_cap_latest_frame 갱신 ──
static void capture_thread_fn()
{
    setup_low_latency_capture_env();

    cv::VideoCapture cap;
    bool opened = false;

    // 1순위: GStreamer UDP (최저 지연)
    const std::string gst_udp = build_gstreamer_rtsp_pipeline(false);
    if (!gst_udp.empty())
    {
        std::cerr << "[camera_RBF] open via GStreamer(UDP)\n";
        opened = cap.open(gst_udp, cv::CAP_GSTREAMER);
    }

    // 2순위: GStreamer TCP
    if (!opened)
    {
        const std::string gst_tcp = build_gstreamer_rtsp_pipeline(true);
        if (!gst_tcp.empty())
        {
            std::cerr << "[camera_RBF] open via GStreamer(TCP)\n";
            opened = cap.open(gst_tcp, cv::CAP_GSTREAMER);
        }
    }

    // 3순위: 기본 백엔드(FFmpeg) fallback
    if (!opened)
    {
        std::cerr << "[camera_RBF] GStreamer open failed, fallback to default backend\n";
        opened = cap.open(RTSP_URL);
    }

    if (!opened || !cap.isOpened())
    {
        std::cerr << "[camera_RBF] RTSP open fail: " << RTSP_URL << "\n";
        g_running = false;
        g_cap_cv.notify_all();
        return;
    }

    std::cerr << "[camera_RBF] RTSP open ok: " << RTSP_URL << "\n";
    try { std::cerr << "[camera_RBF] capture backend=" << cap.getBackendName() << "\n"; }
    catch (...) {}

    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

    while (g_running)
    {
        cv::Mat f;
        if (!cap.read(f) || f.empty())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(g_cap_mutex);
            g_cap_latest_frame = std::move(f);
            g_cap_seq++;
            g_cap_last_ts = std::chrono::steady_clock::now();
        }
        g_cap_cv.notify_one();
    }

    cap.release();
    g_cap_cv.notify_all();
}

enum class RemoteTrackEvent
{
    None,
    Start,
    Pos,
    End
};

static bool parse_track_event_and_id(const std::string& line, RemoteTrackEvent& out_evt, std::string& out_id)
{
    std::string s = line;
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    std::size_t p0 = 0;
    while (p0 < s.size() && (s[p0] == ' ' || s[p0] == '\t')) p0++;
    if (p0 > 0) s = s.substr(p0);
    if (s.empty()) return false;

    auto extract_id_after = [&](const std::string& token) -> std::string {
        std::size_t p = s.find(token);
        if (p == std::string::npos) return "";
        p += token.size();
        std::size_t e = p;
        while (e < s.size() && s[e] != '|' && s[e] != ',' && s[e] != ' ' && s[e] != '\t' && s[e] != '\r' && s[e] != '\n') e++;
        return s.substr(p, e - p);
    };

    out_evt = RemoteTrackEvent::None;
    out_id.clear();

    // +IPD prefix나 기타 문자열이 앞에 붙어도 find()로 처리한다.
    if (s.find("TRACK_POS|") != std::string::npos)
    {
        out_evt = RemoteTrackEvent::Pos;
        out_id = extract_id_after("TRACK_POS|");
        return !out_id.empty();
    }
    if (s.find("TRACK_START|") != std::string::npos)
    {
        out_evt = RemoteTrackEvent::Start;
        out_id = extract_id_after("TRACK_START|");
        return !out_id.empty();
    }
    if (s.find("TRACK_END|") != std::string::npos)
    {
        out_evt = RemoteTrackEvent::End;
        out_id = extract_id_after("TRACK_END|");
        return !out_id.empty();
    }

    // 보조 포맷(직접 id만 보내는 경우)
    if (s.rfind("SELECT_ID|", 0) == 0)
    {
        out_evt = RemoteTrackEvent::Pos;
        out_id = extract_id_after("SELECT_ID|");
        return !out_id.empty();
    }
    if (s.rfind("ID=", 0) == 0 || s.rfind("id=", 0) == 0)
    {
        out_evt = RemoteTrackEvent::Pos;
        out_id = (s.rfind("ID=", 0) == 0) ? s.substr(3) : s.substr(3);
        return !out_id.empty();
    }
    // plain line: "123" 형태도 허용
    out_evt = RemoteTrackEvent::Pos;
    out_id = s;
    return !out_id.empty();
}

static bool parse_track_bbox_norm(const std::string& line, double& l, double& t, double& r, double& b)
{
    // TRACK_POS|id|L=...|T=...|R=...|B=...
    auto pick = [&](const char* key, double& out) -> bool {
        std::size_t p = line.find(key);
        if (p == std::string::npos) return false;
        p += std::strlen(key);
        std::size_t e = p;
        while (e < line.size() && line[e] != '|' && line[e] != ',' && line[e] != ' ' && line[e] != '\t' &&
               line[e] != '\r' && line[e] != '\n') e++;
        try {
            out = std::stod(line.substr(p, e - p));
            return true;
        } catch (...) {
            return false;
        }
    };

    double ll = 0, tt = 0, rr = 0, bb = 0;
    if (!(pick("L=", ll) && pick("T=", tt) && pick("R=", rr) && pick("B=", bb)))
        return false;

    // 값 스케일 자동 보정: 0~1이면 그대로, 그 외는 센서 좌표로 간주
    if (std::max({std::fabs(ll), std::fabs(tt), std::fabs(rr), std::fabs(bb)}) > 2.0)
    {
        ll /= SENSOR_WIDTH;  rr /= SENSOR_WIDTH;
        tt /= SENSOR_HEIGHT; bb /= SENSOR_HEIGHT;
    }
    ll = std::max(0.0, std::min(1.0, ll));
    rr = std::max(0.0, std::min(1.0, rr));
    tt = std::max(0.0, std::min(1.0, tt));
    bb = std::max(0.0, std::min(1.0, bb));
    if (rr <= ll || bb <= tt) return false;
    l = ll; t = tt; r = rr; b = bb;
    return true;
}

static bool obj_bbox_norm(const ParsedMetadataObject& obj, double& l, double& t, double& r, double& b)
{
    double ll = obj.left, rr = obj.right, tt = obj.top, bb = obj.bottom;
    if (std::max({std::fabs(ll), std::fabs(rr), std::fabs(tt), std::fabs(bb)}) > 2.0)
    {
        ll /= SENSOR_WIDTH;  rr /= SENSOR_WIDTH;
        tt /= SENSOR_HEIGHT; bb /= SENSOR_HEIGHT;
    }
    ll = std::max(0.0, std::min(1.0, ll));
    rr = std::max(0.0, std::min(1.0, rr));
    tt = std::max(0.0, std::min(1.0, tt));
    bb = std::max(0.0, std::min(1.0, bb));
    if (rr <= ll || bb <= tt) return false;
    l = ll; t = tt; r = rr; b = bb;
    return true;
}

static double iou_norm(double l1, double t1, double r1, double b1,
                       double l2, double t2, double r2, double b2)
{
    const double ix1 = std::max(l1, l2);
    const double iy1 = std::max(t1, t2);
    const double ix2 = std::min(r1, r2);
    const double iy2 = std::min(b1, b2);
    const double iw = std::max(0.0, ix2 - ix1);
    const double ih = std::max(0.0, iy2 - iy1);
    const double inter = iw * ih;
    if (inter <= 0.0) return 0.0;
    const double a1 = std::max(0.0, r1 - l1) * std::max(0.0, b1 - t1);
    const double a2 = std::max(0.0, r2 - l2) * std::max(0.0, b2 - t2);
    const double uni = a1 + a2 - inter;
    if (uni <= 1e-9) return 0.0;
    return inter / uni;
}

static void remote_select_thread_fn(std::string host, int port)
{
    constexpr int kReconnectMs = 700;
    while (g_running)
    {
#ifdef _WIN32
        SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == INVALID_SOCKET)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectMs));
            continue;
        }
#else
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectMs));
            continue;
        }
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        {
            std::cerr << "[remote_select] invalid host ip: " << host << "\n";
#ifdef _WIN32
            ::closesocket(fd);
#else
            ::close(fd);
#endif
            return;
        }
#ifdef _WIN32
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
#else
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
#endif
        {
#ifdef _WIN32
            ::closesocket(fd);
#else
            ::close(fd);
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectMs));
            continue;
        }
        std::cerr << "[remote_select] connected " << host << ":" << port << "\n";
        // QT 모드: 연결된 소켓 fd를 역방향 PWM 전송에 사용
        {
            std::lock_guard<std::mutex> lk(g_remote_client_fd_mutex);
            g_remote_client_fd = fd;
        }

        std::string buf;
        char tmp[512];
        while (g_running)
        {
#ifdef _WIN32
            int n = ::recv(fd, tmp, static_cast<int>(sizeof(tmp)), 0);
#else
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
#endif
            if (n <= 0) break;
            buf.append(tmp, static_cast<std::size_t>(n));
            while (true)
            {
                std::size_t eol = buf.find_first_of("\r\n");
                if (eol == std::string::npos) break;
                std::string line = buf.substr(0, eol);
                std::size_t cut = eol;
                while (cut < buf.size() && (buf[cut] == '\r' || buf[cut] == '\n')) cut++;
                buf.erase(0, cut);

                RemoteTrackEvent evt = RemoteTrackEvent::None;
                std::string rid;
                if (!parse_track_event_and_id(line, evt, rid)) continue;

                if (evt == RemoteTrackEvent::Pos || evt == RemoteTrackEvent::Start)
                {
                    double rl = 0, rt = 0, rr = 0, rb = 0;
                    const bool has_bbox = parse_track_bbox_norm(line, rl, rt, rr, rb);
                    // 클릭 선택과 동일한 경로: selected_id를 해당 id로 갱신
                    {
                        std::lock_guard<std::mutex> lk(g_sel_mutex);
                        g_selected_id = rid;
                        g_selected_valid = true;
                        g_selected_rect = cv::Rect();
                    }
                    {
                        std::lock_guard<std::mutex> lk(g_click_mutex);
                        g_click_pending = true;
                        g_click_pending_id = rid;
                    }
                    {
                        std::lock_guard<std::mutex> lk(g_remote_msg_mutex);
                        g_remote_last_msg = line;
                        g_remote_last_id = rid;
                        g_remote_tracking = true;
                        g_remote_bbox_valid = has_bbox;
                        if (has_bbox)
                        {
                            g_remote_l = rl; g_remote_t = rt; g_remote_r = rr; g_remote_b = rb;
                        }
                        g_remote_last_rx_tp = std::chrono::steady_clock::now();
                    }
                    std::cerr << "[remote_select] TRACK id=" << rid << "\n";
                }
                else if (evt == RemoteTrackEvent::End)
                {
                    {
                        std::lock_guard<std::mutex> lk(g_remote_msg_mutex);
                        g_remote_last_msg = line;
                        g_remote_last_id = rid;
                        g_remote_tracking = false;
                        g_remote_bbox_valid = false;
                        g_remote_last_rx_tp = std::chrono::steady_clock::now();
                    }
                    {
                        std::lock_guard<std::mutex> lk(g_sel_mutex);
                        if (g_selected_valid && g_selected_id == rid)
                        {
                            g_selected_valid = false;
                            g_selected_id.clear();
                            g_selected_rect = cv::Rect();
                        }
                    }
                    std::cerr << "[remote_select] TRACK_END id=" << rid << "\n";
                }
            }
        }
        // 연결 해제: fd 클리어
        {
            std::lock_guard<std::mutex> lk(g_remote_client_fd_mutex);
#ifdef _WIN32
            g_remote_client_fd = INVALID_SOCKET;
#else
            g_remote_client_fd = -1;
#endif
        }
#ifdef _WIN32
        ::closesocket(fd);
#else
        ::close(fd);
#endif
        if (g_running)
            std::cerr << "[remote_select] disconnected, reconnecting...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectMs));
    }
}

// ──────────────────────────────────────────────────────────────────────
// DeepSORT 워커 프로세스 + 비동기 스레드
// ──────────────────────────────────────────────────────────────────────
struct DeepSortWorker {
#ifdef _WIN32
    PROCESS_INFORMATION pi{};
    HANDLE child_stdin_write{NULL};
    HANDLE child_stdout_read{NULL};
#else
    pid_t pid{-1};
    int   write_fd{-1};
    int   read_fd{-1};
#endif
    bool  active{false};

    // 비동기 스레드용
    std::thread worker_thread;
    std::mutex  input_mutex;
    std::condition_variable input_cv;
    bool input_ready{false};
    cv::Mat pending_frame;
    std::vector<ParsedMetadataObject> pending_objs;

    // 결과 저장
    std::mutex  result_mutex;
    std::vector<ParsedMetadataObject> latest_result;
    std::atomic<bool> dead_log_once{false};

    std::string get_exe_dir() const {
#ifdef _WIN32
        char buf[MAX_PATH];
        DWORD n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
        if (n == 0 || n == MAX_PATH) return "";
        return std::filesystem::path(buf).parent_path().string();
#else
        char buf[4096];
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0) return "";
        buf[n] = '\0';
        return std::filesystem::path(buf).parent_path().string();
#endif
    }

    bool start() {
        if (active) return true;
        std::string root = get_exe_dir();
        if (root.empty()) return false;
#ifdef _WIN32
        std::string py_exec = "python";
        {
            std::filesystem::path py1 = std::filesystem::path(root) / ".venv" / "Scripts" / "python.exe";
            std::filesystem::path py2 = std::filesystem::path(root).parent_path() / ".venv" / "Scripts" / "python.exe";
            std::filesystem::path py3 = std::filesystem::path(root).parent_path().parent_path() / ".venv" / "Scripts" / "python.exe";
            if (std::filesystem::exists(py1))
                py_exec = py1.string();
            else if (std::filesystem::exists(py2))
                py_exec = py2.string();
            else if (std::filesystem::exists(py3))
                py_exec = py3.string();
        }
#else
        // python(venv) / script 경로는 실행 위치(root)에 따라 달라질 수 있으므로
        // 여러 후보를 순서대로 시도하고, 없으면 system python3로 폴백한다.
        std::string py_exec = "python3";
        {
            std::filesystem::path py1 = std::filesystem::path(root) / ".venv" / "bin" / "python";
            std::filesystem::path py2 = std::filesystem::path(root).parent_path() / ".venv" / "bin" / "python";
            std::filesystem::path py3 = std::filesystem::path(root).parent_path().parent_path() / ".venv" / "bin" / "python";
            if (std::filesystem::exists(py1))
                py_exec = py1.string();
            else if (std::filesystem::exists(py2))
                py_exec = py2.string();
            else if (std::filesystem::exists(py3))
                py_exec = py3.string();
        }
#endif

        // 스크립트 위치 후보
        std::filesystem::path script;
        {
            std::filesystem::path s1 = std::filesystem::path(root) / "src" / "deepsort_tracker_worker.py";
            std::filesystem::path s2 = std::filesystem::path(root).parent_path() / "src" / "deepsort_tracker_worker.py";
            std::filesystem::path s3 = std::filesystem::path(root).parent_path() / "get_metadata" / "src" / "deepsort_tracker_worker.py";
            if (std::filesystem::exists(s1))
                script = s1;
            else if (std::filesystem::exists(s2))
                script = s2;
            else if (std::filesystem::exists(s3))
                script = s3;
        }

        if (script.empty() || !std::filesystem::exists(script)) {
            std::cerr << "[deepsort] worker script missing\n"
                      << "  root=" << root << "\n"
                      << "  script candidates: root/src/, parent/src/, parent/get_metadata/src/\n";
            return false;
        }
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE child_stdout_read_tmp = NULL;
        HANDLE child_stdout_write = NULL;
        HANDLE child_stdin_read = NULL;
        HANDLE child_stdin_write_tmp = NULL;

        if (!CreatePipe(&child_stdout_read_tmp, &child_stdout_write, &sa, 0)) return false;
        if (!SetHandleInformation(child_stdout_read_tmp, HANDLE_FLAG_INHERIT, 0)) return false;
        if (!CreatePipe(&child_stdin_read, &child_stdin_write_tmp, &sa, 0)) return false;
        if (!SetHandleInformation(child_stdin_write_tmp, HANDLE_FLAG_INHERIT, 0)) return false;

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = child_stdin_read;
        si.hStdOutput = child_stdout_write;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        std::string cmd = "\"" + py_exec + "\" \"" + script.string() + "\"";
        std::vector<char> cmdline(cmd.begin(), cmd.end());
        cmdline.push_back('\0');

        BOOL ok = CreateProcessA(
            nullptr,
            cmdline.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi
        );

        CloseHandle(child_stdin_read);
        CloseHandle(child_stdout_write);
        if (!ok)
        {
            CloseHandle(child_stdout_read_tmp);
            CloseHandle(child_stdin_write_tmp);
            return false;
        }
        child_stdout_read = child_stdout_read_tmp;
        child_stdin_write = child_stdin_write_tmp;
        active = true;
        std::cerr << "[deepsort] worker started pid=" << pi.dwProcessId << "\n";
#else
        int pipe_in[2], pipe_out[2];
        if (::pipe(pipe_in) || ::pipe(pipe_out)) return false;
        pid = ::fork();
        if (pid == 0) {
            ::dup2(pipe_in[0],  STDIN_FILENO);
            ::dup2(pipe_out[1], STDOUT_FILENO);
            // 디버깅용: DeepSORT worker stderr를 파일로 남긴다.
            // (기존엔 /dev/null로 덮어버려서 import 에러 등을 확인 못했음)
            const char* errlog = "/tmp/deepsort_worker_stderr.log";
            int dn = ::open(errlog, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (dn >= 0) { ::dup2(dn, STDERR_FILENO); ::close(dn); }
            ::close(pipe_in[0]); ::close(pipe_in[1]);
            ::close(pipe_out[0]); ::close(pipe_out[1]);
            const char* p = py_exec.c_str();
            const char* s = script.c_str();
            char* const argv[] = {const_cast<char*>(p), const_cast<char*>(s), nullptr};
            ::execvp(p, argv);
            _exit(127);
        }
        ::close(pipe_in[0]);
        ::close(pipe_out[1]);
        write_fd = pipe_in[1];
        read_fd  = pipe_out[0];
        active   = true;
        std::cerr << "[deepsort] worker started pid=" << pid << "\n";

        // 워커가 즉시 종료했는지 체크(예: deep_sort_realtime import 실패)
        {
            int status = 0;
            pid_t w = ::waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                active = false;
                std::cerr << "[deepsort] worker exited early. status=" << status
                          << " (stderr: /tmp/deepsort_worker_stderr.log)\n";
                return false;
            }
        }

        // 비동기 처리 스레드 시작
        worker_thread = std::thread([this]() { async_loop(); });

        return true;
#endif
        worker_thread = std::thread([this]() { async_loop(); });
        return true;
    }

    void stop() {
        active = false;
        // 스레드 깨워서 종료
        {
            std::lock_guard<std::mutex> lk(input_mutex);
            input_ready = true;
        }
        input_cv.notify_all();
        if (worker_thread.joinable()) worker_thread.join();
#ifdef _WIN32
        if (child_stdin_write) { CloseHandle(child_stdin_write); child_stdin_write = NULL; }
        if (child_stdout_read) { CloseHandle(child_stdout_read); child_stdout_read = NULL; }
        if (pi.hProcess) { TerminateProcess(pi.hProcess, 0); CloseHandle(pi.hProcess); pi.hProcess = NULL; }
        if (pi.hThread) { CloseHandle(pi.hThread); pi.hThread = NULL; }
#else
        if (write_fd >= 0) { ::close(write_fd); write_fd = -1; }
        if (read_fd  >= 0) { ::close(read_fd);  read_fd  = -1; }
        if (pid > 0) { ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0); pid = -1; }
#endif
    }

    // 메인루프에서 호출 — 논블로킹, 최신 결과만 반환
    std::vector<ParsedMetadataObject> get_latest() {
        std::lock_guard<std::mutex> lk(result_mutex);
        return latest_result;
    }

    // 메인루프에서 호출 — 새 입력 전달 (논블로킹)
    void push(const cv::Mat& frame, const std::vector<ParsedMetadataObject>& objs) {
        std::lock_guard<std::mutex> lk(input_mutex);
        pending_frame = frame.clone();
        pending_objs  = objs;
        input_ready   = true;
        input_cv.notify_one();
    }

private:
    void mark_worker_dead(const char* why)
    {
        if (!active) return;
        active = false;
        if (!dead_log_once.exchange(true))
            std::cerr << "[deepsort] worker disabled: " << why << "\n";
    }

    // 비동기 처리 루프
    void async_loop() {
        while (true) {
            cv::Mat frame;
            std::vector<ParsedMetadataObject> objs;
            {
                std::unique_lock<std::mutex> lk(input_mutex);
                input_cv.wait(lk, [this]{ return input_ready || !active; });
                if (!active) break;
                frame = std::move(pending_frame);
                objs  = std::move(pending_objs);
                input_ready = false;
            }
            if (frame.empty() || objs.empty()) continue;

            auto tracks = send_to_worker(frame, objs);

            std::lock_guard<std::mutex> lk(result_mutex);
            if (!tracks.empty())
                latest_result = std::move(tracks);
        }
    }

    // Python 워커에 실제로 데이터 보내고 결과 받기
    std::vector<ParsedMetadataObject> send_to_worker(
        const cv::Mat& frame,
        const std::vector<ParsedMetadataObject>& raw_objs)
    {
        std::vector<ParsedMetadataObject> result;
        if (frame.empty() || raw_objs.empty()) return result;

        std::vector<uchar> buf;
        std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, 70};
        if (!cv::imencode(".jpg", frame, buf, params)) return result;

        const int W = frame.cols, H = frame.rows;
        struct BBox { float l, t, r, b, conf; };
        std::vector<BBox> bboxes;
        for (const auto& obj : raw_objs) {
            float l, r, t, b;
            if (std::max({obj.left, obj.right, obj.top, obj.bottom}) <= 1.5f) {
                l = obj.left * W; r = obj.right * W;
                t = obj.top  * H; b = obj.bottom * H;
            } else {
                float sx = (float)W / SENSOR_WIDTH;
                float sy = (float)H / SENSOR_HEIGHT;
                l = obj.left * sx; r = obj.right * sx;
                t = obj.top  * sy; b = obj.bottom * sy;
            }
            bboxes.push_back({l, t, r, b, 0.9f});
        }

        uint32_t jpeg_len = (uint32_t)buf.size();
        uint8_t hdr[4];
        hdr[0]=jpeg_len&0xFF; hdr[1]=(jpeg_len>>8)&0xFF;
        hdr[2]=(jpeg_len>>16)&0xFF; hdr[3]=(jpeg_len>>24)&0xFF;
#ifdef _WIN32
        DWORD wrote = 0;
        if (!WriteFile(child_stdin_write, hdr, 4, &wrote, nullptr) || wrote != 4) { mark_worker_dead("stdin header write failed"); return result; }
        DWORD total = 0;
        while (total < jpeg_len) {
            DWORD chunk = 0;
            DWORD remain = jpeg_len - total;
            if (!WriteFile(child_stdin_write, buf.data() + total, remain, &chunk, nullptr) || chunk == 0) { mark_worker_dead("stdin jpeg write failed"); return result; }
            total += chunk;
        }
#else
        if (::write(write_fd, hdr, 4) != 4) { mark_worker_dead("stdin header write failed"); return result; }
        ssize_t total = 0;
        while (total < (ssize_t)jpeg_len) {
            ssize_t w = ::write(write_fd, buf.data() + total, jpeg_len - total);
            if (w <= 0) { mark_worker_dead("stdin jpeg write failed"); return result; }
            total += w;
        }
#endif
        uint32_t bbox_count = (uint32_t)bboxes.size();
        uint8_t bchdr[4];
        bchdr[0]=bbox_count&0xFF; bchdr[1]=(bbox_count>>8)&0xFF;
        bchdr[2]=(bbox_count>>16)&0xFF; bchdr[3]=(bbox_count>>24)&0xFF;
#ifdef _WIN32
        if (!WriteFile(child_stdin_write, bchdr, 4, &wrote, nullptr) || wrote != 4) { mark_worker_dead("stdin bbox header write failed"); return result; }
#else
        if (::write(write_fd, bchdr, 4) != 4) { mark_worker_dead("stdin bbox header write failed"); return result; }
#endif
        for (const auto& bb : bboxes) {
            float vals[5] = {bb.l, bb.t, bb.r, bb.b, bb.conf};
            uint8_t raw[20];
            memcpy(raw, vals, 20);
#ifdef _WIN32
            if (!WriteFile(child_stdin_write, raw, 20, &wrote, nullptr) || wrote != 20) { mark_worker_dead("stdin bbox write failed"); return result; }
#else
            if (::write(write_fd, raw, 20) != 20) { mark_worker_dead("stdin bbox write failed"); return result; }
#endif
        }

        // 응답 읽기 (타임아웃 5초 — 별도 스레드라 블로킹 OK)
#ifdef _WIN32
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::string line;
        char c = 0;
        while (std::chrono::steady_clock::now() < deadline)
        {
            DWORD avail = 0;
            if (!PeekNamedPipe(child_stdout_read, nullptr, 0, nullptr, &avail, nullptr))
            {
                mark_worker_dead("stdout pipe broken");
                return result;
            }
            if (avail == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            DWORD got = 0;
            if (!ReadFile(child_stdout_read, &c, 1, &got, nullptr) || got != 1) { mark_worker_dead("stdout read failed"); return result; }
            if (c == '\n') break;
            line.push_back(c);
        }
        if (line.empty()) { mark_worker_dead("stdout timeout"); return result; }
        if (line[0] == '0') return result;
#else
        pollfd pfd{read_fd, POLLIN, 0};
        if (::poll(&pfd, 1, 5000) <= 0) { mark_worker_dead("stdout timeout/poll fail"); return result; }
        std::string line;
        char c;
        while (true) {
            pollfd pfd2{read_fd, POLLIN, 0};
            if (::poll(&pfd2, 1, 100) <= 0) break;
            if (::read(read_fd, &c, 1) != 1) { mark_worker_dead("stdout read failed"); return result; }
            if (c == '\n') break;
            line.push_back(c);
        }
        if (line.empty()) { mark_worker_dead("stdout empty line"); return result; }
        if (line[0] == '0') return result;
#endif

        std::istringstream iss(line);
        int count;
        if (!(iss >> count)) return result;
        for (int i = 0; i < count; i++) {
            int tid, l, t, r, b;
            if (!(iss >> tid >> l >> t >> r >> b)) break;
            ParsedMetadataObject obj;
            obj.id   = std::to_string(tid);
            obj.type = "Head";
            obj.x    = (l + r) / 2.0f;
            obj.y    = (t + b) / 2.0f;
            float sx = (float)SENSOR_WIDTH  / W;
            float sy = (float)SENSOR_HEIGHT / H;
            obj.left   = l * sx; obj.right  = r * sx;
            obj.top    = t * sy; obj.bottom = b * sy;
            result.push_back(obj);
        }
        return result;
    }
};

static DeepSortWorker g_deepsort;

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

    out.x = std::max(0, std::min(out.x, W - 1));
    out.y = std::max(0, std::min(out.y, H - 1));
    out.width = std::max(1, std::min(out.width, W - out.x));
    out.height = std::max(1, std::min(out.height, H - out.y));
    return (out.area() > 20);
}

static void metadata_thread_fn(RTSPClient* client, XMLParser* parser)
{
    unsigned char header[4];
    char* big_buffer = new char[65536];
    std::string accumulated_xml;
    unsigned int last_timestamp = 0;
    auto sock = client->getSocket();

    while (g_running)
    {
        client->sendHeartbeat();

        int read_len = recv(sock, reinterpret_cast<char*>(header), 4, MSG_WAITALL);
        if (read_len <= 0) break;
        if (header[0] != '$') continue;

        int channel = (int)header[1];
        int payload_len = ((int)header[2] << 8) | (int)header[3];

        int total_read = 0;
        while (total_read < payload_len)
        {
            int to_read = payload_len - total_read;
            if (to_read > 65536) to_read = 65536;
            int r = recv(sock, big_buffer + total_read, to_read, 0);
            if (r <= 0) { total_read = 0; break; }
            total_read += r;
        }
        if (total_read <= 12) continue;

        if (channel == 2)
        {
            unsigned char* rtp_ptr = (unsigned char*)big_buffer;
            unsigned int current_timestamp =
                (rtp_ptr[4] << 24) | (rtp_ptr[5] << 16) | (rtp_ptr[6] << 8) | rtp_ptr[7];
            char* xml_data = big_buffer + 12;
            int xml_len = total_read - 12;

            if (current_timestamp != last_timestamp && last_timestamp != 0)
            {
                auto objs = parser->parseHumanObjectsForAnalytics(accumulated_xml, g_detect_all.load());
                {
                    std::lock_guard<std::mutex> lock(g_raw_obj_mutex);
                    g_raw_objects = std::move(objs);
                }
                accumulated_xml.clear();
            }
            accumulated_xml.append(xml_data, xml_len);
            last_timestamp = current_timestamp;
        }
    }

    delete[] big_buffer;
}

static void on_mouse(int event, int x, int y, int /*flags*/, void* userdata)
{
    if (event != cv::EVENT_LBUTTONDOWN) return;
    // QT 모드: 마우스 클릭으로 추적 선택하지 않음 (Track 버튼으로만 선택)
    if (g_qt_mode) return;

    cv::Mat* frame_ptr = static_cast<cv::Mat*>(userdata);
    cv::Mat frame_copy;
    {
        std::lock_guard<std::mutex> lock(g_frame_mutex);
        if (frame_ptr->empty()) return;
        frame_copy = frame_ptr->clone();
    }
    int W = frame_copy.cols;
    int H = frame_copy.rows;

    // 렌더링·인터랙션에는 stable_id가 들어있는 g_stable_objects 사용
    std::vector<ParsedMetadataObject> objs;
    {
        std::lock_guard<std::mutex> lock(g_stable_mutex);
        objs = g_stable_objects;
    }

    for (const auto& obj : objs)
    {
        cv::Rect rect;
        if (!compute_rect_from_obj(obj, W, H, rect)) continue;
        if (!rect.contains(cv::Point(x, y))) continue;

        {
            std::lock_guard<std::mutex> lock(g_sel_mutex);
            g_selected_id = obj.id;
            g_selected_rect = rect;
            g_selected_valid = true;
        }

        std::cerr << "SELECT "
                  << "id=" << obj.id
                  << " x=" << rect.x << " y=" << rect.y
                  << " w=" << rect.width << " h=" << rect.height
                  << std::endl;

        {
            std::lock_guard<std::mutex> lk(g_click_mutex);
            g_click_pending = true;
            g_click_pending_id = obj.id;
        }
        break;
    }
}

class RbfTps2D
{
public:
    bool fit(const std::vector<cv::Point2d>& X, const std::vector<double>& Y)
    {
        const int N = (int)X.size();
        if (N < 4 || (int)Y.size() != N) return false;

        cv::Mat A = cv::Mat::zeros(N + 3, N + 3, CV_64F);
        cv::Mat b = cv::Mat::zeros(N + 3, 1, CV_64F);

        for (int i = 0; i < N; ++i)
        {
            b.at<double>(i, 0) = Y[i];
            for (int j = 0; j < N; ++j)
            {
                double dx = X[i].x - X[j].x;
                double dy = X[i].y - X[j].y;
                double r = std::sqrt(dx * dx + dy * dy);
                A.at<double>(i, j) = phi(r);
            }
            A.at<double>(i, N + 0) = 1.0;
            A.at<double>(i, N + 1) = X[i].x;
            A.at<double>(i, N + 2) = X[i].y;
        }

        for (int j = 0; j < N; ++j)
        {
            A.at<double>(N + 0, j) = 1.0;
            A.at<double>(N + 1, j) = X[j].x;
            A.at<double>(N + 2, j) = X[j].y;
        }

        cv::Mat x;
        bool ok = cv::solve(A, b, x, cv::DECOMP_SVD);
        if (!ok) return false;

        w_.assign(N, 0.0);
        for (int i = 0; i < N; ++i)
            w_[i] = x.at<double>(i, 0);
        a0_ = x.at<double>(N + 0, 0);
        a1_ = x.at<double>(N + 1, 0);
        a2_ = x.at<double>(N + 2, 0);
        X_ = X;
        return true;
    }

    double eval(double x, double y) const
    {
        const int N = (int)X_.size();
        double s = 0.0;
        for (int i = 0; i < N; ++i)
        {
            double dx = x - X_[i].x;
            double dy = y - X_[i].y;
            double r = std::sqrt(dx * dx + dy * dy);
            s += w_[i] * phi(r);
        }
        s += a0_ + a1_ * x + a2_ * y;
        return s;
    }

private:
    static double phi(double r)
    {
        const double eps = 1e-6;
        double rr = r * r;
        return rr * std::log(r + eps);
    }

    std::vector<cv::Point2d> X_;
    std::vector<double> w_;
    double a0_{0.0}, a1_{0.0}, a2_{0.0};
};

struct CalibPoint
{
    double X_cm{0.0}, Y_cm{0.0};
    double u{0.0}, v{0.0};
    double pan{0.0}, tilt{0.0};
    std::string name;
};

static std::vector<CalibPoint> load_calib_points()
{
    const double data[][6] = {
        {-370,  65,   80,   405,   890,  1320},
        {-370,  201,  401,  285,  1055,  1390},
        {-370,  406,  775,  191,  1220,  1420},
        {-370,  586,  970,  154,  1305,  1435},
        {-370,  880,  1154, 129,  1385,  1450},
        {-370,  990,  1201, 124,  1410,  1455},
        {-508,  990,  1085, 108,  1355,  1460},
        {0,     221,  1607, 550,  1605,  1235},
        {0,     571,  1608, 292,  1600,  1385},
        {0,     891,  1588, 221,  1585,  1430},
        {-234,  0,      5,  632,   870,  1170},
        {-234,  250,  785,  300,  1220,  1360},
        {-234,  560,  1163, 193,  1390,  1420},
        {-234,  870,  1310, 159,  1455,  1440},
        {-234,  1000, 1342, 149,  1470,  1450},
        {0,     100,  1535, 898,  1570,  1065},
    };

    std::vector<CalibPoint> pts;
    const int N = (int)(sizeof(data) / sizeof(data[0]));
    pts.reserve(N);
    for (int i = 0; i < N; ++i)
    {
        CalibPoint p;
        p.X_cm = data[i][0]; p.Y_cm = data[i][1];
        p.u = data[i][2];    p.v = data[i][3];
        p.pan = data[i][4];  p.tilt = data[i][5];
        p.name = "p" + std::to_string(i);
        pts.push_back(p);
    }
    return pts;
}

static void draw_world_grid(cv::Mat& frame, const RbfTps2D& rbf_u, const RbfTps2D& rbf_v,
                            const std::vector<CalibPoint>& pts)
{
    const int W = frame.cols;
    const int H = frame.rows;

    auto world_to_pixel = [&](double X_cm, double Y_cm) -> cv::Point {
        double u = rbf_u.eval(X_cm, Y_cm);
        double v = rbf_v.eval(X_cm, Y_cm);
        return cv::Point((int)std::lround(u), (int)std::lround(v));
    };

    double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
    for (const auto& p : pts)
    {
        minX = std::min(minX, p.X_cm); maxX = std::max(maxX, p.X_cm);
        minY = std::min(minY, p.Y_cm); maxY = std::max(maxY, p.Y_cm);
    }

    const double stepX = 100.0, stepY = 100.0;
    const int samples = 60;

    for (double X = std::floor(minX / stepX) * stepX; X <= maxX + 1e-6; X += stepX)
    {
        std::vector<cv::Point> poly;
        poly.reserve(samples);
        for (int i = 0; i < samples; ++i)
        {
            double t = (double)i / (double)(samples - 1);
            double Y = minY + t * (maxY - minY);
            cv::Point p = world_to_pixel(X, Y);
            if (p.x < -2000 || p.x > W + 2000 || p.y < -2000 || p.y > H + 2000) continue;
            poly.push_back(p);
        }
        if (poly.size() >= 2)
            cv::polylines(frame, poly, false, cv::Scalar(80, 80, 220), 1, cv::LINE_AA);
    }

    for (double Y = std::floor(minY / stepY) * stepY; Y <= maxY + 1e-6; Y += stepY)
    {
        std::vector<cv::Point> poly;
        poly.reserve(samples);
        for (int i = 0; i < samples; ++i)
        {
            double t = (double)i / (double)(samples - 1);
            double X = minX + t * (maxX - minX);
            cv::Point p = world_to_pixel(X, Y);
            if (p.x < -2000 || p.x > W + 2000 || p.y < -2000 || p.y > H + 2000) continue;
            poly.push_back(p);
        }
        if (poly.size() >= 2)
            cv::polylines(frame, poly, false, cv::Scalar(80, 220, 80), 1, cv::LINE_AA);
    }

    for (const auto& p : pts)
    {
        cv::Point uv((int)std::lround(p.u), (int)std::lround(p.v));
        cv::circle(frame, uv, 4, cv::Scalar(0, 200, 255), -1, cv::LINE_AA);
        cv::putText(frame, p.name.c_str(), uv + cv::Point(6, -6),
                    cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0, 200, 255), 1);
    }
}

struct KalmanBbox2D
{
    double cx{0}, cy{0};
    double vx{0}, vy{0};
    double w{0}, h{0};
    double alpha_pos{0.6};
    double beta_vel{0.15};
    double alpha_size{0.3};
    // bbox 측정값이 한 프레임에 크게 튀는 outlier(예: ID/박스 튐)일 때
    // 속도 업데이트를 망가뜨리지 않도록 게이팅을 둔다.
    double max_jump_px{120.0};      // predicted->measured까지 최대 허용 이동(px)
    double max_vel_px_s{2000.0};   // 속도 상한(px/s)
    bool initialized{false};

    void update(double meas_cx, double meas_cy, double meas_w, double meas_h, double dt)
    {
        if (!initialized || dt <= 0)
        {
            cx = meas_cx; cy = meas_cy;
            w = meas_w;   h = meas_h;
            vx = vy = 0;
            initialized = true;
            return;
        }

        // dt가 너무 작으면 (beta_vel*rx)/dt 항이 폭주할 수 있으므로 하한을 건다.
        dt = std::max(dt, 1e-4);

        double px = cx + vx * dt;
        double py = cy + vy * dt;
        double rx = meas_cx - px;
        double ry = meas_cy - py;

        // outlier 게이팅: 측정이 예측에서 너무 멀면 "속도는 신뢰하지 않고" 위치만 갱신.
        const double dist2 = rx * rx + ry * ry;
        if (dist2 > max_jump_px * max_jump_px)
        {
            cx = meas_cx;
            cy = meas_cy;
            w = meas_w;
            h = meas_h;
            vx = 0;
            vy = 0;
            return;
        }

        cx = px + alpha_pos * rx;
        cy = py + alpha_pos * ry;
        vx += (beta_vel * rx) / dt;
        vy += (beta_vel * ry) / dt;

        // velocity 상한으로 pred 흔들림(증폭) 방지
        vx = std::max(-max_vel_px_s, std::min(max_vel_px_s, vx));
        vy = std::max(-max_vel_px_s, std::min(max_vel_px_s, vy));

        w += alpha_size * (meas_w - w);
        h += alpha_size * (meas_h - h);
    }

    void predict(double dt_ahead, double& pred_cx, double& pred_cy) const
    {
        pred_cx = cx + vx * dt_ahead;
        pred_cy = cy + vy * dt_ahead;
    }

    void reset() { initialized = false; cx = cy = vx = vy = w = h = 0; }
};

int main(int argc, char** argv)
{
    enum class TrackerMode { DeepSort, Native };
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return -1;
    }
#endif
    double ratio = 0.35;
    double alpha = 0.5;
    int pan_min = 500, pan_max = 2500;
    int tilt_min = 500, tilt_max = 2500;
    int send_every_n = 1;
    bool draw_grid = true;
    double predict_ms = 300.0;
    int pwm_log_interval_ms = 2000; // 사용자 요청: 터미널 로그만 2초마다
    std::string remote_id_host = "192.168.0.101";
    int remote_id_port = 5565;
    bool remote_id_enable = true;
    TrackerMode trackerMode = TrackerMode::Native;  // 기본: Native (DeepSort는 --tracker-mode deepsort)

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--detect-all") g_detect_all = true;
        else if (arg == "--qt-mode") g_qt_mode = true;
        else if (arg == "--ratio" && i + 1 < argc) ratio = std::atof(argv[++i]);
        else if (arg == "--alpha" && i + 1 < argc) alpha = std::atof(argv[++i]);
        else if (arg == "--send-every" && i + 1 < argc) send_every_n = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--no-grid") draw_grid = false;
        else if (arg == "--predict-ms" && i + 1 < argc) predict_ms = std::atof(argv[++i]);
        else if (arg == "--send-interval-ms" && i + 1 < argc) pwm_log_interval_ms = std::max(100, std::atoi(argv[++i]));
        else if (arg == "--remote-id-host" && i + 1 < argc) remote_id_host = argv[++i];
        else if (arg == "--remote-id-port" && i + 1 < argc) remote_id_port = std::atoi(argv[++i]);
        else if (arg == "--no-remote-id") remote_id_enable = false;
        else if (arg == "--tracker-mode" && i + 1 < argc)
        {
            std::string mode = argv[++i];
            if (mode == "deepsort") trackerMode = TrackerMode::DeepSort;
            else                    trackerMode = TrackerMode::Native;
        }
    }

    std::signal(SIGINT, signal_handler);
#ifndef _WIN32
    std::signal(SIGPIPE, signal_handler);
#endif

    // DeepSORT 워커 시작 (native 모드에서는 사용 안 함)
    if (trackerMode == TrackerMode::DeepSort)
    {
        if (!g_deepsort.start())
            std::cerr << "[deepsort] 워커 시작 실패 — 카메라 ID 그대로 사용\n";
    }
    else
    {
        std::cerr << "[tracker] native mode enabled (IoU greedy + IdStabilizer)\n";
    }

    // Calib points
    std::vector<CalibPoint> pts = load_calib_points();

    std::vector<cv::Point2d> px;
    std::vector<double> pan_y, tilt_y;
    px.reserve(pts.size());
    pan_y.reserve(pts.size());
    tilt_y.reserve(pts.size());
    for (const auto& p : pts)
    {
        px.emplace_back(p.u, p.v);
        pan_y.emplace_back(p.pan);
        tilt_y.emplace_back(p.tilt);
    }
    RbfTps2D rbf_pan, rbf_tilt;
    if (!rbf_pan.fit(px, pan_y) || !rbf_tilt.fit(px, tilt_y))
    {
        std::cerr << "[RBF] fit failed\n";
        return -1;
    }
    std::cerr << "[RBF] fitted N=" << px.size() << "\n";

    std::vector<cv::Point2d> wxy;
    std::vector<double> uu, vv;
    wxy.reserve(pts.size());
    uu.reserve(pts.size());
    vv.reserve(pts.size());
    for (const auto& p : pts)
    {
        wxy.emplace_back(p.X_cm, p.Y_cm);
        uu.emplace_back(p.u);
        vv.emplace_back(p.v);
    }
    RbfTps2D rbf_u, rbf_v;
    if (!rbf_u.fit(wxy, uu) || !rbf_v.fit(wxy, vv))
        std::cerr << "[GRID] world->pixel RBF fit failed (grid disabled)\n";

    // OpenCV(FFmpeg 백엔드)로 RTSP를 읽을 때 지연/버퍼 문제로 cap.read()가 실패하는 경우가 있어
    // camera_client와 동일한 low-latency 옵션을 먼저 걸어둔다.
#ifdef _WIN32
    _putenv_s("OPENCV_FFMPEG_CAPTURE_OPTIONS",
              "rtsp_transport;tcp|fflags;nobuffer|flags;low_delay|max_delay;0");
#else
    setenv("OPENCV_FFMPEG_CAPTURE_OPTIONS",
           "rtsp_transport;tcp|fflags;nobuffer|flags;low_delay|max_delay;0", 1);
#endif

    // 어떤 backend가 사용 가능한지/실제로 뭘 쓰는지 로그로 확실히 남긴다.
    {
        try {
            const auto backs = cv::videoio_registry::getBackends();
            std::cerr << "[videoio] available backends:";
            for (const auto b : backs)
                std::cerr << " " << cv::videoio_registry::getBackendName(b);
            std::cerr << "\n";
        } catch (...) {
            std::cerr << "[videoio] registry query failed (old OpenCV build?)\n";
        }
        std::cerr << "[videoio] OpenCV build info (first lines)\n";
        const std::string bi = cv::getBuildInformation();
        // 너무 길어서 앞부분만
        std::cerr << bi.substr(0, std::min<size_t>(2000, bi.size())) << "\n";
    }

    RTSPClient client;
    XMLParser parser;
    if (!client.connectToCamera())
        return -1;
    client.sendHandshake();
    std::thread meta_thread(metadata_thread_fn, &client, &parser);
    std::thread remote_sel_thread;
    if (remote_id_enable)
    {
        {
            std::lock_guard<std::mutex> lk(g_remote_msg_mutex);
            g_remote_src_host = remote_id_host;
        }
        remote_sel_thread = std::thread(remote_select_thread_fn, remote_id_host, remote_id_port);
    }

    // 캡처 전용 스레드 시작 (GStreamer UDP→TCP→FFmpeg 자동 fallback)
    std::thread cap_thread(capture_thread_fn);

    cv::namedWindow("camera_RBF", cv::WINDOW_NORMAL);
    // OpenCV highgui 스레드를 시작해서 Windows에서 창 생성이 늦는 문제를 완화한다.
    cv::startWindowThread();
    cv::setMouseCallback("camera_RBF", on_mouse, &g_last_frame);

    int prev_pan = 1500, prev_tilt = 1500;
    int frame_id = 0;
    auto t_fps0 = std::chrono::steady_clock::now();
    auto t_last_frame = std::chrono::steady_clock::now();
    auto t_last_pwm_log = std::chrono::steady_clock::now() - std::chrono::milliseconds(pwm_log_interval_ms);

    KalmanBbox2D kf;
    std::string prev_sel_id;

    // NativeTrack 상태 (Native 모드 전용)
    struct NativeTrack
    {
        int id{0};
        cv::Rect2d box;
        cv::Point2d vel{0.0, 0.0};
        int miss{0};
        int lock_det{-1};   // 마지막으로 매칭된 detection index
        int lock_left{0};   // 밀집 상황 ID 보존 잔여 프레임
    };
    std::vector<NativeTrack> nativeTracks;
    int nativeNextId = 1;

    while (g_running)
    {
        cv::Mat frame;
        {
            std::unique_lock<std::mutex> lk(g_cap_mutex);
            g_cap_cv.wait_for(lk, std::chrono::milliseconds(100), [] {
                return !g_running || !g_cap_latest_frame.empty();
            });
            if (!g_running) break;
            if (g_cap_latest_frame.empty()) continue;
            frame = g_cap_latest_frame.clone();
        }
        if (frame.empty()) continue;

        {
            std::lock_guard<std::mutex> lock(g_frame_mutex);
            g_last_frame = frame.clone();
        }

        const int W = frame.cols;
        const int H = frame.rows;

        std::vector<ParsedMetadataObject> raw_objs;
        { std::lock_guard<std::mutex> lock(g_raw_obj_mutex); raw_objs = g_raw_objects; }

        if (trackerMode == TrackerMode::DeepSort)
        {
            // ── DeepSORT 비동기 업데이트 ────────────────────────────
            if (g_deepsort.active && !raw_objs.empty())
                g_deepsort.push(frame, raw_objs);

            auto tracked = g_deepsort.get_latest();
            std::string sel_id_now;
            bool sel_valid_now = false;
            {
                std::lock_guard<std::mutex> lk(g_sel_mutex);
                sel_valid_now = g_selected_valid;
                sel_id_now = g_selected_id;
            }
            std::lock_guard<std::mutex> lock(g_obj_mutex);
            if (!tracked.empty())
            {
                g_deepsort_last_objects = tracked;
                g_deepsort_empty_count = 0;
                g_objects = tracked;
            }
            else
            {
                g_deepsort_empty_count++;
                bool sel_found_in_last = false;
                if (sel_valid_now && !g_deepsort_last_objects.empty())
                    for (const auto& obj : g_deepsort_last_objects)
                        if (obj.id == sel_id_now) { sel_found_in_last = true; break; }

                if ((g_deepsort_empty_count <= DEEPSORT_EMPTY_GRACE_FRAMES) &&
                    !g_deepsort_last_objects.empty() &&
                    (!sel_valid_now || sel_found_in_last))
                    g_objects = g_deepsort_last_objects;
                else
                    g_objects = raw_objs;
            }
        }
        else
        {
            // ── Native C++ tracker: IoU + 중심점 거리 greedy association ──
            struct Det { ParsedMetadataObject obj; cv::Rect2d box; cv::Point2d c; };
            std::vector<Det> dets;
            dets.reserve(raw_objs.size());
            for (const auto& ro : raw_objs)
            {
                cv::Rect r;
                if (!compute_rect_from_obj(ro, W, H, r)) continue;
                Det d;
                d.obj = ro;
                d.box = cv::Rect2d(r.x, r.y, r.width, r.height);
                d.c   = cv::Point2d(d.box.x + d.box.width * 0.5, d.box.y + d.box.height * 0.5);
                dets.push_back(std::move(d));
            }

            std::vector<int>  detOwner(dets.size(), -1);
            std::vector<bool> trackMatched(nativeTracks.size(), false);

            auto iou = [](const cv::Rect2d& a, const cv::Rect2d& b) {
                const double x1 = std::max(a.x, b.x), y1 = std::max(a.y, b.y);
                const double x2 = std::min(a.x + a.width,  b.x + b.width);
                const double y2 = std::min(a.y + a.height, b.y + b.height);
                const double w = std::max(0.0, x2 - x1), h = std::max(0.0, y2 - y1);
                const double inter = w * h;
                if (inter <= 0.0) return 0.0;
                const double uni = a.area() + b.area() - inter;
                return (uni > 1e-9) ? (inter / uni) : 0.0;
            };

            // 밀집 여부 판단
            std::vector<bool> detCrowded(dets.size(), false);
            for (size_t i = 0; i < dets.size(); ++i)
                for (size_t j = i + 1; j < dets.size(); ++j)
                    if (iou(dets[i].box, dets[j].box) > 0.35)
                    { detCrowded[i] = true; detCrowded[j] = true; }

            for (size_t ti = 0; ti < nativeTracks.size(); ++ti)
            {
                auto& tr = nativeTracks[ti];
                cv::Rect2d pred = tr.box;
                pred.x += tr.vel.x;
                pred.y += tr.vel.y;
                const cv::Point2d predC(pred.x + pred.width * 0.5, pred.y + pred.height * 0.5);

                double bestScore = 0.0;
                int bestDi = -1;
                for (size_t di = 0; di < dets.size(); ++di)
                {
                    if (detOwner[di] != -1) continue;
                    const double ov   = iou(pred, dets[di].box);
                    const double dist = cv::norm(predC - dets[di].c);
                    const bool crowded = detCrowded[di];
                    const double distTerm = std::exp(-dist / (crowded ? 80.0 : 120.0));
                    const double score = 0.85 * ov + 0.15 * distTerm;
                    if (score > bestScore) { bestScore = score; bestDi = static_cast<int>(di); }
                }

                if (bestDi >= 0)
                {
                    const bool crowded = detCrowded[bestDi];
                    const double bestOv   = iou(pred, dets[bestDi].box);
                    const double bestDist = cv::norm(predC - dets[bestDi].c);
                    // 밀집 상황에서 더 엄격한 임계값으로 ID 스위칭 억제
                    const double thScore = crowded ? 0.26 : 0.18;
                    const double thIou   = crowded ? 0.20 : 0.10;
                    const double thDist  = crowded ? 95.0 : 180.0;
                    bool accept = (bestScore >= thScore && bestOv >= thIou && bestDist <= thDist);

                    // lock_left 프레임 동안 이전 det을 우선 연결해 겹침 상황 ID 보존
                    if (!accept && tr.lock_left > 0 && tr.lock_det >= 0 &&
                        tr.lock_det < static_cast<int>(dets.size()) &&
                        detOwner[tr.lock_det] == -1)
                    {
                        const double lkOv   = iou(pred, dets[tr.lock_det].box);
                        const double lkDist = cv::norm(predC - dets[tr.lock_det].c);
                        if (lkOv >= 0.12 && lkDist <= 110.0) { bestDi = tr.lock_det; accept = true; }
                    }

                    if (!accept) { tr.lock_left = std::max(0, tr.lock_left - 1); continue; }

                    const cv::Point2d prevC(tr.box.x + tr.box.width * 0.5, tr.box.y + tr.box.height * 0.5);
                    tr.vel      = 0.7 * tr.vel + 0.3 * (dets[bestDi].c - prevC);
                    tr.box      = dets[bestDi].box;
                    tr.miss     = 0;
                    tr.lock_det = bestDi;
                    tr.lock_left = detCrowded[bestDi] ? 4 : std::max(0, tr.lock_left - 1);
                    detOwner[bestDi] = static_cast<int>(ti);
                    trackMatched[ti] = true;
                }
            }

            for (size_t ti = 0; ti < nativeTracks.size(); ++ti)
                if (!trackMatched[ti]) nativeTracks[ti].miss++;

            nativeTracks.erase(
                std::remove_if(nativeTracks.begin(), nativeTracks.end(),
                               [](const NativeTrack& t) { return t.miss > 20; }),
                nativeTracks.end());

            for (size_t di = 0; di < dets.size(); ++di)
            {
                if (detOwner[di] != -1) continue;
                NativeTrack t;
                t.id  = nativeNextId++;
                t.box = dets[di].box;
                t.lock_det = static_cast<int>(di);
                nativeTracks.push_back(t);
                detOwner[di] = static_cast<int>(nativeTracks.size() - 1);
            }

            std::vector<ParsedMetadataObject> trackedNative;
            trackedNative.reserve(dets.size());
            for (size_t di = 0; di < dets.size(); ++di)
            {
                if (detOwner[di] < 0 || detOwner[di] >= static_cast<int>(nativeTracks.size())) continue;
                auto obj = dets[di].obj;
                obj.id   = "N" + std::to_string(nativeTracks[detOwner[di]].id);
                trackedNative.push_back(std::move(obj));
            }
            std::lock_guard<std::mutex> lock(g_obj_mutex);
            g_objects = std::move(trackedNative);
        }

        // copy objs
        std::vector<ParsedMetadataObject> objs;
        {
            std::lock_guard<std::mutex> lock(g_obj_mutex);
            objs = g_objects;
        }

        // ── IdStabilizer 적용: objs의 id를 stable_id로 덮어쓰기 ────
        {
            std::vector<DetectedInput> inputs;
            inputs.reserve(objs.size());
            for (const auto& o : objs) {
                DetectedInput d;
                d.camera_id = o.id;
                d.left = o.left;  d.top = o.top;
                d.right = o.right; d.bottom = o.bottom;
                d.cx = o.x;  d.cy = o.y;
                inputs.push_back(d);
            }
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            auto stable = g_stabilizer.update(inputs, now_ms);

            objs.clear();
            objs.reserve(stable.size());
            for (const auto& s : stable) {
                ParsedMetadataObject obj;
                obj.id     = s.stable_id;
                obj.type   = "Human";
                obj.x      = s.cx;
                obj.y      = s.cy;
                obj.left   = s.left;
                obj.top    = s.top;
                obj.right  = s.right;
                obj.bottom = s.bottom;
                objs.push_back(obj);
            }
        }
        // 렌더링·클릭용 stable 버퍼에만 기록 (g_objects는 트래커 내부 ID 유지)
        {
            std::lock_guard<std::mutex> lock(g_stable_mutex);
            g_stable_objects = objs;
        }

        // ── ID별 EMA 스무딩 적용 후 draw boxes ─────────────────────
        // DeepSORT 비동기 지연으로 인한 프레임 점프, to_ltrb() 칼만 예측값의
        // 순간 튐을 EMA로 감쇠시킨다. 새 ID가 처음 나타날 때는 초기화(직결),
        // 이후 프레임부터 EMA 적용. ID가 사라지면 버퍼에서 제거.
        {
            std::set<std::string> active_ids;
            for (const auto& obj : objs)
            {
                cv::Rect raw_r;
                if (!compute_rect_from_obj(obj, W, H, raw_r)) continue;
                active_ids.insert(obj.id);

                auto& sr = g_bbox_smooth[obj.id];
                if (!sr.init)
                {
                    // 첫 등장: 스무더 초기화 (점프 없이 바로 세팅)
                    sr.x = raw_r.x; sr.y = raw_r.y;
                    sr.w = raw_r.width; sr.h = raw_r.height;
                    sr.init = true;
                }
                else
                {
                    // Adaptive EMA: 위치·크기·종횡비 상태에 따라 alpha를 동적으로 조정
                    const double dx      = raw_r.x     - sr.x;
                    const double dy      = raw_r.y     - sr.y;
                    const double dw      = raw_r.width  - sr.w;
                    const double dh      = raw_r.height - sr.h;
                    const double dist    = std::sqrt(dx*dx + dy*dy);
                    const double size_rel_w = std::fabs(dw) / std::max(1.0, sr.w);
                    const double size_rel_h = std::fabs(dh) / std::max(1.0, sr.h);
                    const double curr_ar = sr.w / std::max(1.0, sr.h);
                    const double raw_ar  = raw_r.width / std::max(1.0, (double)raw_r.height);

                    // 속도·거리 구간별 adaptive alpha
                    double a      = BBOX_SMOOTH_ALPHA;
                    double a_size = BBOX_SMOOTH_ALPHA * 0.75;
                    if      (dist < 2.5)   { a = 0.06; a_size = 0.02; }   // 거의 정지: 크기 고정에 가깝게
                    else if (dist < 8.0)   { a = 0.14; a_size = 0.07; }   // 저속
                    else if (dist > 100.0) { a = 0.15; a_size = 0.10; }   // outlier 점프

                    // deadband: 1px 이하 이동 억제
                    const double ddx = (std::fabs(dx) < 1.0) ? 0.0 : dx;
                    const double ddy = (std::fabs(dy) < 1.0) ? 0.0 : dy;

                    // size deadband: 3% 이하 크기 변화 억제 (미세 깜빡임 제거)
                    const bool size_jitter = (size_rel_w < 0.03 && size_rel_h < 0.03 && dist < 6.0);
                    if (size_jitter) a_size = 0.0;

                    // aspect ratio 급변 가드: bbox가 크게 찌그러지는 현상 억제
                    if (std::fabs(raw_ar - curr_ar) > 0.18 && dist < 10.0)
                        a_size = std::min(a_size, 0.03);

                    sr.x += a * ddx;
                    sr.y += a * ddy;

                    const double prev_w = sr.w;
                    const double prev_h = sr.h;
                    sr.w += a_size * dw;
                    sr.h += a_size * dh;

                    // per-frame 크기 변화 hard clamp (breathing 현상 방지)
                    const double max_step_w = std::max(2.0, prev_w * 0.04);
                    const double max_step_h = std::max(2.0, prev_h * 0.04);
                    sr.w = std::max(1.0, std::min(prev_w + max_step_w, std::max(prev_w - max_step_w, sr.w)));
                    sr.h = std::max(1.0, std::min(prev_h + max_step_h, std::max(prev_h - max_step_h, sr.h)));
                }

                cv::Rect r(
                    (int)std::lround(sr.x), (int)std::lround(sr.y),
                    (int)std::lround(std::max(1.0, sr.w)),
                    (int)std::lround(std::max(1.0, sr.h))
                );
                r.x = std::max(0, std::min(r.x, W - 1));
                r.y = std::max(0, std::min(r.y, H - 1));
                r.width  = std::max(1, std::min(r.width,  W - r.x));
                r.height = std::max(1, std::min(r.height, H - r.y));

                cv::rectangle(frame, r, cv::Scalar(0, 255, 255), 2);
                cv::putText(frame, obj.id.c_str(), cv::Point(r.x, std::max(0, r.y - 5)),
                            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 255), 1);
            }
            // 사라진 ID 스무더 정리 (메모리 누수 방지)
            for (auto it = g_bbox_smooth.begin(); it != g_bbox_smooth.end(); )
            {
                if (active_ids.find(it->first) == active_ids.end())
                    it = g_bbox_smooth.erase(it);
                else
                    ++it;
            }
        }

        // selected rect
        std::string sel_id;
        cv::Rect sel_rect;
        bool sel_ok = false;
        {
            std::lock_guard<std::mutex> lock(g_sel_mutex);
            sel_ok = g_selected_valid;
            sel_id = g_selected_id;
            sel_rect = g_selected_rect;
        }

        if (sel_ok)
        {
            bool found_selected = false;
            for (const auto& obj : objs)
            {
                if (obj.id != sel_id) continue;
                // 스무딩된 bbox를 sel_rect에 반영
                auto it = g_bbox_smooth.find(sel_id);
                if (it != g_bbox_smooth.end() && it->second.init)
                {
                    cv::Rect smoothed(
                        (int)std::lround(it->second.x), (int)std::lround(it->second.y),
                        (int)std::lround(std::max(1.0, it->second.w)),
                        (int)std::lround(std::max(1.0, it->second.h))
                    );
                    smoothed.x = std::max(0, std::min(smoothed.x, W - 1));
                    smoothed.y = std::max(0, std::min(smoothed.y, H - 1));
                    smoothed.width  = std::max(1, std::min(smoothed.width,  W - smoothed.x));
                    smoothed.height = std::max(1, std::min(smoothed.height, H - smoothed.y));
                    sel_rect = smoothed;
                }
                else
                {
                    cv::Rect updated;
                    if (compute_rect_from_obj(obj, W, H, updated))
                        sel_rect = updated;
                }
                {
                    std::lock_guard<std::mutex> lock(g_sel_mutex);
                    g_selected_rect = sel_rect;
                }
                found_selected = true;
                break;
            }

            // 원격 ID가 XML(raw) 기준인데 DeepSORT ID와 다를 수 있으므로 raw에서도 한번 더 찾는다.
            if (!found_selected)
            {
                for (const auto& obj : raw_objs)
                {
                    if (obj.id != sel_id) continue;
                    cv::Rect updated;
                    if (compute_rect_from_obj(obj, W, H, updated))
                    {
                        sel_rect = updated;
                        {
                            std::lock_guard<std::mutex> lock(g_sel_mutex);
                            g_selected_rect = sel_rect;
                        }
                        found_selected = true;
                    }
                    break;
                }
            }

            // ID가 다르면 TRACK_POS 박스와 IoU가 가장 큰 객체를 자동 매칭
            if (!found_selected)
            {
                bool has_remote_bbox = false;
                double rl = 0, rt = 0, rr = 0, rb = 0;
                {
                    std::lock_guard<std::mutex> lk(g_remote_msg_mutex);
                    has_remote_bbox = g_remote_bbox_valid;
                    rl = g_remote_l; rt = g_remote_t; rr = g_remote_r; rb = g_remote_b;
                }
                if (has_remote_bbox)
                {
                    const ParsedMetadataObject* best_obj = nullptr;
                    double best_iou = 0.0;
                    auto eval_best = [&](const std::vector<ParsedMetadataObject>& cand) {
                        for (const auto& obj : cand)
                        {
                            double l, t, r, b;
                            if (!obj_bbox_norm(obj, l, t, r, b)) continue;
                            const double iou = iou_norm(rl, rt, rr, rb, l, t, r, b);
                            if (iou > best_iou)
                            {
                                best_iou = iou;
                                best_obj = &obj;
                            }
                        }
                    };
                    eval_best(objs);
                    if (!best_obj) eval_best(raw_objs);

                    if (best_obj && best_iou >= 0.10)
                    {
                        cv::Rect updated;
                        if (compute_rect_from_obj(*best_obj, W, H, updated))
                        {
                            sel_rect = updated;
                            sel_id = best_obj->id;
                            sel_ok = true;
                            found_selected = true;
                            {
                                std::lock_guard<std::mutex> lock(g_sel_mutex);
                                g_selected_id = sel_id;
                                g_selected_valid = true;
                                g_selected_rect = sel_rect;
                            }
                            std::cerr << "[remote_select] REMAP by IoU old_id=" << prev_sel_id
                                      << " -> xml_id=" << sel_id
                                      << " iou=" << std::fixed << std::setprecision(3) << best_iou << "\n";
                        }
                    }
                }
            }

            // 선택된 ID를 이번 프레임에 못 찾으면 클릭 동작과 동일하게 선택 해제
            if (!found_selected)
            {
                sel_ok = false;
                {
                    std::lock_guard<std::mutex> lock(g_sel_mutex);
                    g_selected_valid = false;
                    g_selected_rect = cv::Rect();
                }
            }
        }

        auto t_now = std::chrono::steady_clock::now();
        double dt_sec = std::chrono::duration<double>(t_now - t_last_frame).count();
        t_last_frame = t_now;
        if (dt_sec <= 0 || dt_sec > 1.0) dt_sec = 1.0 / 30.0;
        // dt가 튀면 속도 추정이 흔들릴 수 있어 범위를 제한한다.
        dt_sec = std::max(1.0 / 120.0, std::min(1.0 / 15.0, dt_sec));

        if (sel_ok && sel_id != prev_sel_id)
        {
            kf.reset();
            prev_pan = 1500;
            prev_tilt = 1500;
            prev_sel_id = sel_id;
        }
        if (!sel_ok && !prev_sel_id.empty())
        {
            kf.reset();
            prev_sel_id.clear();
        }

        int target_u = W / 2;
        int target_v = H / 2;
        int pred_target_u = target_u;
        int pred_target_v = target_v;
        std::string src = "none";

        if (sel_ok)
        {
            sel_rect.x = std::max(0, std::min(sel_rect.x, W - 1));
            sel_rect.y = std::max(0, std::min(sel_rect.y, H - 1));
            sel_rect.width = std::max(1, std::min(sel_rect.width, W - sel_rect.x));
            sel_rect.height = std::max(1, std::min(sel_rect.height, H - sel_rect.y));

            cv::rectangle(frame, sel_rect, cv::Scalar(0, 255, 0), 2);
            cv::putText(frame, ("SEL " + sel_id).c_str(), cv::Point(sel_rect.x, std::max(0, sel_rect.y - 10)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);

            double bbox_cx = sel_rect.x + sel_rect.width * 0.5;
            double bbox_cy = sel_rect.y + sel_rect.height * ratio;
            bbox_cy = std::max(0.0, std::min((double)(H - 1), bbox_cy));

            target_u = (int)std::lround(bbox_cx);
            target_v = (int)std::lround(bbox_cy);
            src = "kalman";

            kf.update(bbox_cx, bbox_cy, (double)sel_rect.width, (double)sel_rect.height, dt_sec);

            double pcx, pcy;
            kf.predict(predict_ms / 1000.0, pcx, pcy);
            pcx = std::max(0.0, std::min((double)(W - 1), pcx));
            pcy = std::max(0.0, std::min((double)(H - 1), pcy));
            pred_target_u = (int)std::lround(pcx);
            pred_target_v = (int)std::lround(pcy);

            cv::line(frame, cv::Point(sel_rect.x, target_v), cv::Point(sel_rect.x + sel_rect.width, target_v),
                     cv::Scalar(0, 255, 0), 1);
        }

        double pan_d = rbf_pan.eval((double)pred_target_u, (double)pred_target_v);
        double tilt_d = rbf_tilt.eval((double)pred_target_u, (double)pred_target_v);

        int pan = (int)std::lround(std::max((double)pan_min, std::min((double)pan_max, pan_d)));
        int tilt = (int)std::lround(std::max((double)tilt_min, std::min((double)tilt_max, tilt_d)));

        pan = (int)std::lround(alpha * pan + (1.0 - alpha) * prev_pan);
        tilt = (int)std::lround(alpha * tilt + (1.0 - alpha) * prev_tilt);
        prev_pan = pan;
        prev_tilt = tilt;

        {
            std::lock_guard<std::mutex> lk(g_pwm_mutex);
            g_last_pan = pan;
            g_last_tilt = tilt;
            g_last_target_u = pred_target_u;
            g_last_target_v = pred_target_v;
            g_last_pwm_valid = true;
        }

        const auto t_now_send = std::chrono::steady_clock::now();
        if (sel_ok && frame_id % send_every_n == 0)
        {
            std::cout << "SET_PWM,PAN=" << pan << ",TILT=" << tilt << std::endl;
            // QT 모드: 연결된 Qt 클라이언트 소켓으로 PWM 값 역방향 전송
            if (g_qt_mode)
            {
                std::string pwm_msg = "PWM_OUT,PAN=" + std::to_string(pan)
                                      + ",TILT=" + std::to_string(tilt) + "\n";
                std::lock_guard<std::mutex> lk(g_remote_client_fd_mutex);
#ifdef _WIN32
                if (g_remote_client_fd != INVALID_SOCKET)
                    ::send(g_remote_client_fd, pwm_msg.c_str(), (int)pwm_msg.size(), 0);
#else
                if (g_remote_client_fd >= 0)
                    ::send(g_remote_client_fd, pwm_msg.c_str(), pwm_msg.size(), MSG_NOSIGNAL);
#endif
            }
            const bool can_log_by_time =
                (std::chrono::duration_cast<std::chrono::milliseconds>(t_now_send - t_last_pwm_log).count() >= pwm_log_interval_ms);
            if (can_log_by_time)
            {
                std::cerr << "[PWM] id=" << sel_id
                          << " target=(" << pred_target_u << "," << pred_target_v << ")"
                          << " PAN=" << pan << " TILT=" << tilt << "\n";
                t_last_pwm_log = t_now_send;
            }
        }
        else if (frame_id % 30 == 0)
            std::cout << std::endl;
        if (!std::cout)
        {
            g_running = false;
            break;
        }

        {
            std::lock_guard<std::mutex> lk(g_click_mutex);
            if (g_click_pending && sel_ok && sel_id == g_click_pending_id)
            {
                std::cerr << "CLICK_PWM "
                          << "id=" << sel_id
                          << " meas=(" << target_u << "," << target_v << ")"
                          << " pred=(" << pred_target_u << "," << pred_target_v << ")"
                          << " vel=(" << std::fixed << std::setprecision(1) << kf.vx << "," << kf.vy << ")"
                          << " PAN=" << pan << " TILT=" << tilt
                          << std::endl;
                g_click_pending = false;
                g_click_pending_id.clear();
            }
        }

        if (draw_grid)
            draw_world_grid(frame, rbf_u, rbf_v, pts);

        cv::circle(frame, cv::Point(target_u, target_v), 5, cv::Scalar(0, 165, 255), -1, cv::LINE_AA);
        if (sel_ok && kf.initialized)
        {
            cv::line(frame, cv::Point(target_u, target_v), cv::Point(pred_target_u, pred_target_v),
                     cv::Scalar(255, 0, 255), 2, cv::LINE_AA);
            cv::circle(frame, cv::Point(pred_target_u, pred_target_v), 9, cv::Scalar(255, 0, 255), -1, cv::LINE_AA);
            cv::circle(frame, cv::Point(pred_target_u, pred_target_v), 9, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
        }
        else
        {
            cv::circle(frame, cv::Point(target_u, target_v), 8, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
        }

        char info[256];
        std::snprintf(info, sizeof(info), "src=%s predict=%.0fms pan=%d tilt=%d vx=%.0f vy=%.0f",
                      src.c_str(), predict_ms, pan, tilt, kf.vx, kf.vy);
        cv::putText(frame, info, cv::Point(10, 30),
                    cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 255), 2);

        {
            std::string rx_msg;
            std::string rx_id;
            std::string rx_host;
            bool tracking_on = false;
            double age_sec = 999.0;
            {
                std::lock_guard<std::mutex> lk(g_remote_msg_mutex);
                rx_msg = g_remote_last_msg;
                rx_id = g_remote_last_id;
                rx_host = g_remote_src_host;
                tracking_on = g_remote_tracking;
                age_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_remote_last_rx_tp).count();
            }
            if (!rx_msg.empty() && age_sec < 8.0)
            {
                char rx_line[512];
                std::snprintf(rx_line, sizeof(rx_line), "REMOTE(%s) 수신 메세지: %s",
                              rx_host.c_str(), rx_msg.c_str());
                cv::putText(frame, rx_line, cv::Point(10, 58),
                            cv::FONT_HERSHEY_SIMPLEX, 0.52, cv::Scalar(255, 255, 255), 2);
                char trk_line[256];
                std::snprintf(trk_line, sizeof(trk_line), "TRACKING: %s (id=%s)",
                              tracking_on ? "ON" : "OFF", rx_id.empty() ? "none" : rx_id.c_str());
                cv::putText(frame, trk_line, cv::Point(10, 82),
                            cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);
            }
        }

        frame_id++;
        if (frame_id % 30 == 0)
        {
            auto t1 = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(t1 - t_fps0).count();
            double fps = (dt > 1e-6) ? (30.0 / dt) : 0.0;
            t_fps0 = t1;
            std::cerr << "[FPS] " << fps << "\n";
        }

        cv::imshow("camera_RBF", frame);
        int key = cv::waitKey(1) & 0xFF;
        if (key == 27 || key == 'q')
        {
            g_running = false;
            break;
        }
    }

    g_running = false;
    g_cap_cv.notify_all();
    g_deepsort.stop();
    if (cap_thread.joinable()) cap_thread.join();
    if (remote_sel_thread.joinable()) remote_sel_thread.join();
    if (meta_thread.joinable()) meta_thread.join();
    cv::destroyAllWindows();
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}