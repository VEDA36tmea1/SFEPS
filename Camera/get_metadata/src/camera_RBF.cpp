// camera_RBF.cpp
// - ONVIF 메타데이터로 Head bbox 표시/클릭 선택
// - DeepSORT Python 워커로 안정적 ID 부여
// - 선택된 bbox에서 v = top + h * RATIO 지점을 RBF로 PWM 보간
// - stdout: "SET_PWM,PAN=...,TILT=..."

#include "RTSPClient.h"
#include "XMLParser.h"
#include "Config.h"

#include <opencv2/opencv.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <cmath>

#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_detect_all{false};

// ── 파싱된 원시 bbox (카메라 ID 그대로) ─────────────────────────────
static std::mutex g_raw_obj_mutex;
static std::vector<ParsedMetadataObject> g_raw_objects;

// ── DeepSORT가 부여한 안정적 ID bbox ────────────────────────────────
static std::mutex g_obj_mutex;
static std::vector<ParsedMetadataObject> g_objects;

static cv::Mat g_last_frame;
static std::mutex g_frame_mutex;

static std::mutex g_sel_mutex;
static std::string g_selected_id;
static cv::Rect   g_selected_rect;
static bool       g_selected_valid = false;

static std::mutex g_pwm_mutex;
static int  g_last_pan = 1500, g_last_tilt = 1500;
static int  g_last_target_u = -1, g_last_target_v = -1;
static bool g_last_pwm_valid = false;

static std::mutex g_click_mutex;
static bool        g_click_pending = false;
static std::string g_click_pending_id;

static void signal_handler(int) { g_running = false; }

// ──────────────────────────────────────────────────────────────────────
// DeepSORT 워커 프로세스
// ──────────────────────────────────────────────────────────────────────
struct DeepSortWorker {
    pid_t pid{-1};
    int   write_fd{-1};
    int   read_fd{-1};
    bool  active{false};

    std::string get_exe_dir() const {
        char buf[4096];
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0) return "";
        buf[n] = '\0';
        return std::filesystem::path(buf).parent_path().string();
    }

    bool start() {
        if (active) return true;
        std::string root = get_exe_dir();
        if (root.empty()) return false;

        // venv python 경로
        auto py = std::filesystem::path(root) / ".venv" / "bin" / "python";
        if (!std::filesystem::exists(py))
            py = std::filesystem::path(root).parent_path() / ".venv" / "bin" / "python";
        auto script = std::filesystem::path(root) / "src" / "deepsort_tracker_worker.py";

        if (!std::filesystem::exists(py) || !std::filesystem::exists(script)) {
            std::cerr << "[deepsort] worker files missing\n"
                      << "  py=" << py << "\n  script=" << script << "\n";
            return false;
        }

        int pipe_in[2], pipe_out[2];
        if (::pipe(pipe_in) || ::pipe(pipe_out)) return false;

        pid = ::fork();
        if (pid == 0) {
            ::dup2(pipe_in[0],  STDIN_FILENO);
            ::dup2(pipe_out[1], STDOUT_FILENO);
            int dn = ::open("/dev/null", O_WRONLY);
            if (dn >= 0) { ::dup2(dn, STDERR_FILENO); ::close(dn); }
            ::close(pipe_in[0]); ::close(pipe_in[1]);
            ::close(pipe_out[0]); ::close(pipe_out[1]);
            const char* p = py.c_str();
            const char* s = script.c_str();
            char* const argv[] = {const_cast<char*>(p), const_cast<char*>(s), nullptr};
            ::execv(p, argv);
            _exit(127);
        }
        ::close(pipe_in[0]);
        ::close(pipe_out[1]);
        write_fd = pipe_in[1];
        read_fd  = pipe_out[0];
        active   = true;

        // 워커가 즉시 죽는 경우(예: python/모듈 문제)는
        // stderr가 /dev/null로 가려져도 parent 로그로는 확인이 필요하다.
        int status = 0;
        pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid)
        {
            active = false;
            ::close(write_fd); write_fd = -1;
            ::close(read_fd);  read_fd  = -1;
            std::cerr << "[deepsort] worker exited early pid=" << pid
                      << " status=" << status << "\n";
            pid = -1;
            return false;
        }

        std::cerr << "[deepsort] worker started pid=" << pid << "\n";
        return true;
    }

    void stop() {
        active = false;
        if (write_fd >= 0) { ::close(write_fd); write_fd = -1; }
        if (read_fd  >= 0) { ::close(read_fd);  read_fd  = -1; }
        if (pid > 0) { ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0); pid = -1; }
    }

    // frame + bbox 리스트 전송 → DeepSORT ID 반환
    // 반환: [(track_id, left, top, right, bottom), ...]
    using TrackList = std::vector<std::tuple<std::string,int,int,int,int>>;

    TrackList update(const cv::Mat& frame,
                     const std::vector<ParsedMetadataObject>& raw_objs) {
        TrackList result;
        if (!active || frame.empty() || raw_objs.empty()) return result;

        // 1. JPEG 인코딩
        std::vector<uchar> buf;
        std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, 70};
        if (!cv::imencode(".jpg", frame, buf, params)) return result;

        // 2. 픽셀 좌표로 변환
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

        // 3. 전송: [jpeg_len(4)] [jpeg] [bbox_count(4)] [bbox × 20bytes each]
        uint32_t jpeg_len = (uint32_t)buf.size();
        uint8_t hdr[4];
        hdr[0] = jpeg_len & 0xFF; hdr[1] = (jpeg_len >> 8) & 0xFF;
        hdr[2] = (jpeg_len >> 16) & 0xFF; hdr[3] = (jpeg_len >> 24) & 0xFF;
        if (::write(write_fd, hdr, 4) != 4) return result;
        ssize_t total = 0;
        while (total < (ssize_t)jpeg_len) {
            ssize_t w = ::write(write_fd, buf.data() + total, jpeg_len - total);
            if (w <= 0) return result;
            total += w;
        }
        uint32_t bbox_count = (uint32_t)bboxes.size();
        uint8_t bchdr[4];
        bchdr[0] = bbox_count & 0xFF; bchdr[1] = (bbox_count >> 8) & 0xFF;
        bchdr[2] = (bbox_count >> 16) & 0xFF; bchdr[3] = (bbox_count >> 24) & 0xFF;
        if (::write(write_fd, bchdr, 4) != 4) return result;
        for (const auto& bb : bboxes) {
            float vals[5] = {bb.l, bb.t, bb.r, bb.b, bb.conf};
            uint8_t raw[20];
            memcpy(raw, vals, 20);
            if (::write(write_fd, raw, 20) != 20) return result;
        }

        // 4. 응답 읽기 (타임아웃 200ms)
        pollfd pfd{read_fd, POLLIN, 0};
        if (::poll(&pfd, 1, 200) <= 0) return result;

        std::string line;
        char c;
        while (true) {
            pollfd pfd2{read_fd, POLLIN, 0};
            if (::poll(&pfd2, 1, 50) <= 0) break;
            if (::read(read_fd, &c, 1) != 1) break;
            if (c == '\n') break;
            line.push_back(c);
        }
        if (line.empty() || line[0] == '0') return result;

        // 5. 파싱: "N id0 l0 t0 r0 b0 id1 ..."
        std::istringstream iss(line);
        int count;
        if (!(iss >> count)) return result;
        for (int i = 0; i < count; i++) {
            int tid, l, t, r, b;
            if (!(iss >> tid >> l >> t >> r >> b)) break;
            result.emplace_back(std::to_string(tid), l, t, r, b);
        }
        return result;
    }
};

static DeepSortWorker g_deepsort;

// ──────────────────────────────────────────────────────────────────────
static bool compute_rect_from_obj(const ParsedMetadataObject& obj, int W, int H, cv::Rect& out) {
    int left, right, top, bottom;
    if (std::max({obj.left, obj.right, obj.top, obj.bottom}) <= 1.5f) {
        left   = (int)(obj.left   * W); right  = (int)(obj.right  * W);
        top    = (int)(obj.top    * H); bottom = (int)(obj.bottom * H);
    } else {
        double sx = (double)W / SENSOR_WIDTH, sy = (double)H / SENSOR_HEIGHT;
        left   = (int)(obj.left   * sx); right  = (int)(obj.right  * sx);
        top    = (int)(obj.top    * sy); bottom = (int)(obj.bottom * sy);
    }
    out = cv::Rect(left, top, std::max(1, right-left), std::max(1, bottom-top));
    out.x = std::max(0, std::min(out.x, W-1));
    out.y = std::max(0, std::min(out.y, H-1));
    out.width  = std::max(1, std::min(out.width,  W - out.x));
    out.height = std::max(1, std::min(out.height, H - out.y));
    return (out.area() > 20);
}

// ──────────────────────────────────────────────────────────────────────
// 메타데이터 수신 스레드 — raw bbox만 파싱해서 g_raw_objects에 저장
// ──────────────────────────────────────────────────────────────────────
static void metadata_thread_fn(RTSPClient* client, XMLParser* parser) {
    unsigned char header[4];
    char* big_buffer = new char[65536];
    std::string accumulated_xml;
    unsigned int last_timestamp = 0;
    int sock = client->getSocket();

    while (g_running) {
        client->sendHeartbeat();
        int read_len = recv(sock, header, 4, MSG_WAITALL);
        if (read_len <= 0) break;
        if (header[0] != '$') continue;

        int channel = (int)header[1];
        int payload_len = ((int)header[2] << 8) | (int)header[3];
        int total_read = 0;
        while (total_read < payload_len) {
            int to_read = std::min(payload_len - total_read, 65536);
            int r = recv(sock, big_buffer + total_read, to_read, 0);
            if (r <= 0) { total_read = 0; break; }
            total_read += r;
        }
        if (total_read <= 12) continue;

        if (channel == 2) {
            unsigned char* rtp = (unsigned char*)big_buffer;
            unsigned int cur_ts = (rtp[4]<<24)|(rtp[5]<<16)|(rtp[6]<<8)|rtp[7];
            if (cur_ts != last_timestamp && last_timestamp != 0) {
                auto objs = parser->parseHumanObjectsForAnalytics(accumulated_xml, g_detect_all.load());
                {
                    std::lock_guard<std::mutex> lock(g_raw_obj_mutex);
                    g_raw_objects = std::move(objs);
                }
                accumulated_xml.clear();
            }
            accumulated_xml.append(big_buffer + 12, total_read - 12);
            last_timestamp = cur_ts;
        }
    }
    delete[] big_buffer;
}

// ──────────────────────────────────────────────────────────────────────
// 마우스 콜백
// ──────────────────────────────────────────────────────────────────────
static void on_mouse(int event, int x, int y, int, void* userdata) {
    if (event != cv::EVENT_LBUTTONDOWN) return;
    cv::Mat* fp = static_cast<cv::Mat*>(userdata);
    cv::Mat fc;
    { std::lock_guard<std::mutex> lock(g_frame_mutex); if (fp->empty()) return; fc = fp->clone(); }
    int W = fc.cols, H = fc.rows;

    std::vector<ParsedMetadataObject> objs;
    { std::lock_guard<std::mutex> lock(g_obj_mutex); objs = g_objects; }

    for (const auto& obj : objs) {
        cv::Rect rect;
        if (!compute_rect_from_obj(obj, W, H, rect)) continue;
        if (!rect.contains(cv::Point(x, y))) continue;
        { std::lock_guard<std::mutex> lock(g_sel_mutex);
          g_selected_id = obj.id; g_selected_rect = rect; g_selected_valid = true; }
        std::cerr << "SELECT id=" << obj.id << "\n";
        { std::lock_guard<std::mutex> lk(g_click_mutex);
          g_click_pending = true; g_click_pending_id = obj.id; }
        break;
    }
}

// ──────────────────────────────────────────────────────────────────────
// RBF (Thin-Plate Spline)
// ──────────────────────────────────────────────────────────────────────
class RbfTps2D {
public:
    bool fit(const std::vector<cv::Point2d>& X, const std::vector<double>& Y) {
        const int N = (int)X.size();
        if (N < 4 || (int)Y.size() != N) return false;
        cv::Mat A = cv::Mat::zeros(N+3, N+3, CV_64F);
        cv::Mat b = cv::Mat::zeros(N+3, 1,   CV_64F);
        for (int i = 0; i < N; i++) {
            b.at<double>(i,0) = Y[i];
            for (int j = 0; j < N; j++) {
                double dx = X[i].x-X[j].x, dy = X[i].y-X[j].y;
                A.at<double>(i,j) = phi(std::sqrt(dx*dx+dy*dy));
            }
            A.at<double>(i,N)=1; A.at<double>(i,N+1)=X[i].x; A.at<double>(i,N+2)=X[i].y;
        }
        for (int j = 0; j < N; j++) {
            A.at<double>(N,j)=1; A.at<double>(N+1,j)=X[j].x; A.at<double>(N+2,j)=X[j].y;
        }
        cv::Mat x; if (!cv::solve(A,b,x,cv::DECOMP_SVD)) return false;
        w_.assign(N,0); for (int i=0;i<N;i++) w_[i]=x.at<double>(i,0);
        a0_=x.at<double>(N,0); a1_=x.at<double>(N+1,0); a2_=x.at<double>(N+2,0);
        X_=X; return true;
    }
    double eval(double x, double y) const {
        double s=a0_+a1_*x+a2_*y;
        for (int i=0;i<(int)X_.size();i++) {
            double dx=x-X_[i].x, dy=y-X_[i].y;
            s+=w_[i]*phi(std::sqrt(dx*dx+dy*dy));
        }
        return s;
    }
private:
    static double phi(double r) { const double e=1e-6; return r*r*std::log(r+e); }
    std::vector<cv::Point2d> X_; std::vector<double> w_;
    double a0_{0},a1_{0},a2_{0};
};

struct CalibPoint { double X_cm,Y_cm,u,v,pan,tilt; std::string name; };

static std::vector<CalibPoint> load_calib_points() {
    const double data[][6] = {
        {-370,65,80,405,890,1320},{-370,201,401,285,1055,1390},
        {-370,406,775,191,1220,1420},{-370,586,970,154,1305,1435},
        {-370,880,1154,129,1385,1450},{-370,990,1201,124,1410,1455},
        {-508,990,1085,108,1355,1460},{0,221,1607,550,1605,1235},
        {0,571,1608,292,1600,1385},{0,891,1588,221,1585,1430},
        {-234,0,5,632,870,1170},{-234,250,785,300,1220,1360},
        {-234,560,1163,193,1390,1420},{-234,870,1310,159,1455,1440},
        {-234,1000,1342,149,1470,1450},{0,100,1535,898,1570,1065},
    };
    std::vector<CalibPoint> pts;
    for (int i=0;i<16;i++) {
        pts.push_back({data[i][0],data[i][1],data[i][2],data[i][3],
                       data[i][4],data[i][5],"p"+std::to_string(i)});
    }
    return pts;
}

static void draw_world_grid(cv::Mat& frame, const RbfTps2D& ru, const RbfTps2D& rv,
                             const std::vector<CalibPoint>& pts) {
    int W=frame.cols, H=frame.rows;
    auto w2p=[&](double X,double Y){
        return cv::Point((int)std::lround(ru.eval(X,Y)),(int)std::lround(rv.eval(X,Y)));
    };
    double minX=1e9,maxX=-1e9,minY=1e9,maxY=-1e9;
    for(auto&p:pts){minX=std::min(minX,p.X_cm);maxX=std::max(maxX,p.X_cm);
                    minY=std::min(minY,p.Y_cm);maxY=std::max(maxY,p.Y_cm);}
    const int S=60;
    for(double X=std::floor(minX/100)*100;X<=maxX+1e-6;X+=100){
        std::vector<cv::Point> poly;
        for(int i=0;i<S;i++){double t=(double)i/(S-1);double Y=minY+t*(maxY-minY);
            auto p=w2p(X,Y);if(p.x<-2000||p.x>W+2000||p.y<-2000||p.y>H+2000)continue;poly.push_back(p);}
        if(poly.size()>=2) cv::polylines(frame,poly,false,cv::Scalar(80,80,220),1,cv::LINE_AA);
    }
    for(double Y=std::floor(minY/100)*100;Y<=maxY+1e-6;Y+=100){
        std::vector<cv::Point> poly;
        for(int i=0;i<S;i++){double t=(double)i/(S-1);double X=minX+t*(maxX-minX);
            auto p=w2p(X,Y);if(p.x<-2000||p.x>W+2000||p.y<-2000||p.y>H+2000)continue;poly.push_back(p);}
        if(poly.size()>=2) cv::polylines(frame,poly,false,cv::Scalar(80,220,80),1,cv::LINE_AA);
    }
    for(auto&p:pts){
        cv::Point uv((int)std::lround(p.u),(int)std::lround(p.v));
        cv::circle(frame,uv,4,cv::Scalar(0,200,255),-1,cv::LINE_AA);
        cv::putText(frame,p.name,uv+cv::Point(6,-6),cv::FONT_HERSHEY_SIMPLEX,0.4,cv::Scalar(0,200,255),1);
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
    void predict(double dt,double&px,double&py)const{px=cx+vx*dt;py=cy+vy*dt;}
    void reset(){initialized=false;cx=cy=vx=vy=w=h=0;}
};

int main(int argc, char** argv)
{
    double ratio = 0.35;
    double alpha = 0.5;
    int pan_min = 500, pan_max = 2500;
    int tilt_min = 500, tilt_max = 2500;
    int send_every_n = 1;
    bool draw_grid = true;
    double predict_ms = 300.0;

    for(int i=1;i<argc;i++){
        std::string a=argv[i];
        if(a=="--detect-all") g_detect_all=true;
        else if(a=="--ratio"&&i+1<argc)      ratio=std::atof(argv[++i]);
        else if(a=="--alpha"&&i+1<argc)      alpha=std::atof(argv[++i]);
        else if(a=="--send-every"&&i+1<argc) send_every_n=std::max(1,std::atoi(argv[++i]));
        else if(a=="--no-grid")              draw_grid=false;
        else if(a=="--predict-ms"&&i+1<argc) predict_ms=std::atof(argv[++i]);
    }

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGPIPE, signal_handler);

    // DeepSORT 워커 시작
    if (!g_deepsort.start())
        std::cerr << "[deepsort] 워커 시작 실패 — 카메라 ID 그대로 사용\n";

    // RBF fit
    auto pts = load_calib_points();
    std::vector<cv::Point2d> px; std::vector<double> pan_y,tilt_y,uu,vv;
    std::vector<cv::Point2d> wxy;
    for(auto&p:pts){
        px.emplace_back(p.u,p.v); pan_y.push_back(p.pan); tilt_y.push_back(p.tilt);
        wxy.emplace_back(p.X_cm,p.Y_cm); uu.push_back(p.u); vv.push_back(p.v);
    }
    RbfTps2D rbf_pan,rbf_tilt,rbf_u,rbf_v;
    if(!rbf_pan.fit(px,pan_y)||!rbf_tilt.fit(px,tilt_y)){std::cerr<<"[RBF] fit failed\n";return -1;}
    std::cerr<<"[RBF] fitted N="<<px.size()<<"\n";
    if(!rbf_u.fit(wxy,uu)||!rbf_v.fit(wxy,vv)) std::cerr<<"[GRID] fit failed\n";

    // RTSP
    RTSPClient client; XMLParser parser;
    if(!client.connectToCamera()) return -1;
    client.sendHandshake();
    std::thread meta_thread(metadata_thread_fn,&client,&parser);

    cv::VideoCapture cap(RTSP_URL);
    if(!cap.isOpened()){
        std::cerr<<"[camera_RBF] RTSP open fail\n";
        g_running=false; meta_thread.join(); return -1;
    }

    cv::namedWindow("camera_RBF",cv::WINDOW_NORMAL);
    cv::setMouseCallback("camera_RBF",on_mouse,&g_last_frame);

    int prev_pan=1500,prev_tilt=1500,frame_id=0;
    auto t_fps0=std::chrono::steady_clock::now();
    auto t_last_frame=std::chrono::steady_clock::now();
    KalmanBbox2D kf; std::string prev_sel_id;

    while(g_running){
        cv::Mat frame;
        if(!cap.read(frame)||frame.empty()) break;
        { std::lock_guard<std::mutex> lock(g_frame_mutex); g_last_frame=frame.clone(); }

        const int W=frame.cols, H=frame.rows;

        // ── DeepSORT 업데이트 ───────────────────────────────────────
        std::vector<ParsedMetadataObject> raw_objs;
        { std::lock_guard<std::mutex> lock(g_raw_obj_mutex); raw_objs=g_raw_objects; }

        if(g_deepsort.active && !raw_objs.empty()){
            auto tracks = g_deepsort.update(frame, raw_objs);
            std::vector<ParsedMetadataObject> tracked_objs;
            for(auto&[tid,l,t,r,b]:tracks){
                ParsedMetadataObject obj;
                obj.id   = tid;
                obj.type = "Head";
                obj.x    = (l+r)/2.0f;
                obj.y    = (t+b)/2.0f;
                // 픽셀 좌표를 센서 좌표로 역변환
                float sx=(float)SENSOR_WIDTH/W, sy=(float)SENSOR_HEIGHT/H;
                obj.left   = l*sx; obj.right  = r*sx;
                obj.top    = t*sy; obj.bottom = b*sy;
                tracked_objs.push_back(obj);
            }
            // DeepSORT는 track.is_confirmed() 이후에만 반환하므로,
            // 초기 워밍업/타임아웃 구간에서는 tracks가 비어 화면 표시가 안 될 수 있다.
            // 이 경우 raw bbox로 fallback 하여 "표시"부터 복구한다.
            if (tracked_objs.empty())
            {
                { std::lock_guard<std::mutex> lock(g_obj_mutex); g_objects = raw_objs; }
            }
            else
            {
                { std::lock_guard<std::mutex> lock(g_obj_mutex); g_objects = std::move(tracked_objs); }
            }
        } else {
            // 워커 없으면 raw 그대로 사용
            std::lock_guard<std::mutex> lock(g_obj_mutex); g_objects=raw_objs;
        }

        // ── bbox 그리기 ─────────────────────────────────────────────
        std::vector<ParsedMetadataObject> objs;
        { std::lock_guard<std::mutex> lock(g_obj_mutex); objs=g_objects; }
        for(auto&obj:objs){
            cv::Rect r; if(!compute_rect_from_obj(obj,W,H,r)) continue;
            cv::rectangle(frame,r,cv::Scalar(0,255,255),2);
            cv::putText(frame,obj.id,cv::Point(r.x,std::max(0,r.y-5)),
                        cv::FONT_HERSHEY_SIMPLEX,0.5,cv::Scalar(0,255,255),1);
        }

        // ── 선택 객체 추적 ──────────────────────────────────────────
        std::string sel_id; cv::Rect sel_rect; bool sel_ok=false;
        { std::lock_guard<std::mutex> lock(g_sel_mutex);
          sel_ok=g_selected_valid; sel_id=g_selected_id; sel_rect=g_selected_rect; }

        if(sel_ok){
            for(auto&obj:objs){
                if(obj.id==sel_id){
                    cv::Rect upd; if(compute_rect_from_obj(obj,W,H,upd)){
                        sel_rect=upd;
                        std::lock_guard<std::mutex> lock(g_sel_mutex); g_selected_rect=upd;
                    } break;
                }
            }
        }

        auto t_now = std::chrono::steady_clock::now();
        double dt_sec = std::chrono::duration<double>(t_now - t_last_frame).count();
        t_last_frame = t_now;
        if (dt_sec <= 0 || dt_sec > 1.0) dt_sec = 1.0 / 30.0;
        // dt가 튀면 속도 추정이 흔들릴 수 있어 범위를 제한한다.
        dt_sec = std::max(1.0 / 120.0, std::min(1.0 / 15.0, dt_sec));

        if(sel_ok&&sel_id!=prev_sel_id){ kf.reset(); prev_pan=1500; prev_tilt=1500; prev_sel_id=sel_id; }
        if(!sel_ok&&!prev_sel_id.empty()){ kf.reset(); prev_sel_id.clear(); }

        int target_u=W/2,target_v=H/2,pred_u=W/2,pred_v=H/2;
        std::string src="none";

        if(sel_ok){
            sel_rect.x=std::max(0,std::min(sel_rect.x,W-1));
            sel_rect.y=std::max(0,std::min(sel_rect.y,H-1));
            sel_rect.width=std::max(1,std::min(sel_rect.width,W-sel_rect.x));
            sel_rect.height=std::max(1,std::min(sel_rect.height,H-sel_rect.y));

            cv::rectangle(frame,sel_rect,cv::Scalar(0,255,0),2);
            cv::putText(frame,("SEL "+sel_id),cv::Point(sel_rect.x,std::max(0,sel_rect.y-10)),
                        cv::FONT_HERSHEY_SIMPLEX,0.6,cv::Scalar(0,255,0),2);

            double bcx=sel_rect.x+sel_rect.width*0.5;
            double bcy=std::max(0.0,std::min((double)(H-1),(double)sel_rect.y+sel_rect.height*ratio));
            target_u=(int)std::lround(bcx); target_v=(int)std::lround(bcy); src="kalman";

            kf.update(bcx,bcy,(double)sel_rect.width,(double)sel_rect.height,dt_sec);
            double pcx,pcy; kf.predict(predict_ms/1000.0,pcx,pcy);
            pcx=std::max(0.0,std::min((double)(W-1),pcx));
            pcy=std::max(0.0,std::min((double)(H-1),pcy));
            pred_u=(int)std::lround(pcx); pred_v=(int)std::lround(pcy);

            cv::line(frame,cv::Point(sel_rect.x,target_v),
                     cv::Point(sel_rect.x+sel_rect.width,target_v),cv::Scalar(0,255,0),1);
        }

        double pan_d=rbf_pan.eval(pred_u,pred_v);
        double tilt_d=rbf_tilt.eval(pred_u,pred_v);
        int pan=(int)std::lround(std::max((double)pan_min,std::min((double)pan_max,pan_d)));
        int tilt=(int)std::lround(std::max((double)tilt_min,std::min((double)tilt_max,tilt_d)));
        pan=(int)std::lround(alpha*pan+(1.0-alpha)*prev_pan);
        tilt=(int)std::lround(alpha*tilt+(1.0-alpha)*prev_tilt);
        prev_pan=pan; prev_tilt=tilt;

        { std::lock_guard<std::mutex> lk(g_pwm_mutex);
          g_last_pan=pan; g_last_tilt=tilt;
          g_last_target_u=pred_u; g_last_target_v=pred_v; g_last_pwm_valid=true; }

        if(sel_ok&&frame_id%send_every_n==0)
            std::cout<<"SET_PWM,PAN="<<pan<<",TILT="<<tilt<<std::endl;
        else if(frame_id%30==0)
            std::cout<<std::endl;
        if(!std::cout){g_running=false;break;}

        { std::lock_guard<std::mutex> lk(g_click_mutex);
          if(g_click_pending&&sel_ok&&sel_id==g_click_pending_id){
              std::cerr<<"CLICK_PWM id="<<sel_id<<" PAN="<<pan<<" TILT="<<tilt<<"\n";
              g_click_pending=false; g_click_pending_id.clear();
          }
        }

        if(draw_grid) draw_world_grid(frame,rbf_u,rbf_v,pts);

        cv::circle(frame,cv::Point(target_u,target_v),5,cv::Scalar(0,165,255),-1,cv::LINE_AA);
        if(sel_ok&&kf.initialized){
            cv::line(frame,cv::Point(target_u,target_v),cv::Point(pred_u,pred_v),cv::Scalar(255,0,255),2,cv::LINE_AA);
            cv::circle(frame,cv::Point(pred_u,pred_v),9,cv::Scalar(255,0,255),-1,cv::LINE_AA);
            cv::circle(frame,cv::Point(pred_u,pred_v),9,cv::Scalar(255,255,255),2,cv::LINE_AA);
        } else {
            cv::circle(frame,cv::Point(target_u,target_v),8,cv::Scalar(255,255,255),2,cv::LINE_AA);
        }

        char info[256];
        std::snprintf(info,sizeof(info),"src=%s predict=%.0fms pan=%d tilt=%d vx=%.0f vy=%.0f",
                      src.c_str(),predict_ms,pan,tilt,kf.vx,kf.vy);
        cv::putText(frame,info,cv::Point(10,30),cv::FONT_HERSHEY_SIMPLEX,0.7,cv::Scalar(0,255,255),2);

        frame_id++;
        if(frame_id%30==0){
            auto t1=std::chrono::steady_clock::now();
            double dt=std::chrono::duration<double>(t1-t_fps0).count();
            std::cerr<<"[FPS] "<<(dt>1e-6?30.0/dt:0.0)<<"\n";
            t_fps0=t1;
        }

        cv::imshow("camera_RBF",frame);
        int key=cv::waitKey(1)&0xFF;
        if(key==27||key=='q'){g_running=false;break;}
    }

    g_running=false;
    g_deepsort.stop();
    if(meta_thread.joinable()) meta_thread.join();
    cap.release();
    cv::destroyAllWindows();
    return 0;
}