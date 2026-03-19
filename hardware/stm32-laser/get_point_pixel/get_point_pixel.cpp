// get_point_pixel.cpp
//
// RTSP(또는 동영상/카메라) 프레임을 띄우고, 15개 캘리브 포인트에 대해 픽셀 좌표를 순차 저장한다.
// - 좌클릭: 현재 포인트의 (u,v) 저장 + 다음 포인트로 이동 (즉시 파일 저장)
// - 우클릭: 직전 포인트 1개 취소(삭제) + 그 포인트로 되돌아감 (즉시 파일 저장)
// - q / ESC: 종료
//
// 저장 포맷: OpenCV FileStorage YAML (calib_pixels.yml)
// 재실행: 파일이 있으면 로드해서 "다음 미완료 포인트"부터 이어서 진행.

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

struct CalibPoint
{
    std::string id;
    double X_cm{0.0};
    double Y_cm{0.0};
    bool   done{false};
    double u_px{0.0};
    double v_px{0.0};
};

static std::vector<CalibPoint> default_points()
{
    // 단위: cm
    return {
        {"p0",  -370,    65, false, 0, 0},
        {"p1",  -370,  201, false, 0, 0},
        {"p2",  -370,  406, false, 0, 0},
        {"p3",  -370,  586, false, 0, 0},
        {"p4",  -370,  880, false, 0, 0},
        {"p5",  -370,  990, false, 0, 0},
        {"p6",  -508,  990, false, 0, 0},
        {"p7",     0,  221, false, 0, 0},
        {"p8",     0,  571, false, 0, 0},
        {"p9",     0,  891, false, 0, 0},
        {"p10", -234,    0, false, 0, 0},
        {"p11", -234,  250, false, 0, 0},
        {"p12", -234,  560, false, 0, 0},
        {"p13", -234,  870, false, 0, 0},
        {"p14", -234, 1000, false, 0, 0},
        {"p15", 0, 100, false, 0, 0},
    };
}

static bool save_yaml(const std::string& path, const std::vector<CalibPoint>& pts, const std::string& source)
{
    cv::FileStorage fs(path, cv::FileStorage::WRITE);
    if (!fs.isOpened())
        return false;

    fs << "source" << source;
    fs << "points" << "[";
    for (const auto& p : pts)
    {
        fs << "{"
           << "id" << p.id
           << "X_cm" << p.X_cm
           << "Y_cm" << p.Y_cm
           << "done" << (int)p.done
           << "u_px" << p.u_px
           << "v_px" << p.v_px
           << "}";
    }
    fs << "]";
    fs.release();
    return true;
}

static bool load_yaml(const std::string& path, std::vector<CalibPoint>& pts, std::string& source_out)
{
    cv::FileStorage fs(path, cv::FileStorage::READ);
    if (!fs.isOpened())
        return false;

    source_out = (std::string)fs["source"];
    cv::FileNode n = fs["points"];
    if (n.type() != cv::FileNode::SEQ)
        return false;

    // id 기준으로 덮어쓰기(기본 포인트 세트 유지)
    for (auto it = n.begin(); it != n.end(); ++it)
    {
        std::string id = (std::string)(*it)["id"];
        for (auto& p : pts)
        {
            if (p.id == id)
            {
                p.X_cm = (double)(*it)["X_cm"];
                p.Y_cm = (double)(*it)["Y_cm"];
                p.done = ((int)(*it)["done"]) != 0;
                p.u_px = (double)(*it)["u_px"];
                p.v_px = (double)(*it)["v_px"];
                break;
            }
        }
    }
    fs.release();
    return true;
}

static int next_index(const std::vector<CalibPoint>& pts)
{
    for (int i = 0; i < (int)pts.size(); ++i)
        if (!pts[i].done)
            return i;
    return (int)pts.size();
}

struct AppState
{
    std::vector<CalibPoint> pts;
    int idx{0};
    std::string out_path;
    std::string source;
    bool changed{false};
};

static void on_mouse(int event, int x, int y, int /*flags*/, void* userdata)
{
    auto* st = static_cast<AppState*>(userdata);
    if (!st) return;

    if (event == cv::EVENT_LBUTTONDOWN)
    {
        if (st->idx >= (int)st->pts.size())
            return;
        auto& p = st->pts[st->idx];
        p.u_px = (double)x;
        p.v_px = (double)y;
        p.done = true;
        st->changed = true;
        st->idx = next_index(st->pts);
        (void)save_yaml(st->out_path, st->pts, st->source);
        return;
    }
    if (event == cv::EVENT_RBUTTONDOWN)
    {
        // 가장 최근 done 포인트를 하나 취소
        int last_done = -1;
        for (int i = (int)st->pts.size() - 1; i >= 0; --i)
        {
            if (st->pts[i].done)
            {
                last_done = i;
                break;
            }
        }
        if (last_done >= 0)
        {
            st->pts[last_done].done = false;
            st->pts[last_done].u_px = 0.0;
            st->pts[last_done].v_px = 0.0;
            st->idx = last_done;
            st->changed = true;
            (void)save_yaml(st->out_path, st->pts, st->source);
        }
        return;
    }
}

int main(int argc, char** argv)
{
    std::string source = "rtsp://admin:CCgbdCCgbd@192.168.0.84/profile2/media.smp";
    std::string out_path = "calib_pixels.yml";

    // 사용:
    //   ./get_point_pixel [source] [out_path]
    if (argc >= 2) source = argv[1];
    if (argc >= 3) out_path = argv[2];

    AppState st;
    st.pts = default_points();
    st.out_path = out_path;
    st.source = source;

    if (std::filesystem::exists(out_path))
    {
        std::string saved_source;
        if (load_yaml(out_path, st.pts, saved_source))
        {
            st.idx = next_index(st.pts);
            std::cerr << "[get_point_pixel] resume from: " << out_path
                      << " (next idx=" << st.idx << "/" << st.pts.size() << ")\n";
        }
        else
        {
            std::cerr << "[get_point_pixel] WARN: failed to load existing " << out_path
                      << " (start fresh)\n";
            st.idx = next_index(st.pts);
        }
    }
    else
    {
        st.idx = next_index(st.pts);
        (void)save_yaml(out_path, st.pts, st.source); // 초기 파일 생성
    }

    cv::VideoCapture cap;
    if (source.find("!") != std::string::npos)
        cap.open(source, cv::CAP_GSTREAMER);
    else
        cap.open(source);

    if (!cap.isOpened())
    {
        std::cerr << "[get_point_pixel] ERROR: cannot open source: " << source << "\n";
        return 1;
    }

    const std::string win = "get_point_pixel (L=save, R=undo, q/ESC=quit)";
    cv::namedWindow(win, cv::WINDOW_NORMAL);
    cv::setMouseCallback(win, on_mouse, &st);

    cv::Mat frame;
    while (true)
    {
        if (!cap.read(frame) || frame.empty())
        {
            std::cerr << "[get_point_pixel] empty frame\n";
            break;
        }

        // UI overlay
        int done_cnt = 0;
        for (const auto& p : st.pts) if (p.done) ++done_cnt;

        cv::Mat vis = frame;

        // 현재 타겟 포인트 표시
        if (st.idx < (int)st.pts.size())
        {
            const auto& p = st.pts[st.idx];
            std::string msg = "NEXT: " + p.id +
                              " world(cm)=(" + std::to_string((int)p.X_cm) + "," + std::to_string((int)p.Y_cm) + ")" +
                              "  [" + std::to_string(done_cnt) + "/" + std::to_string(st.pts.size()) + "]";
            cv::putText(vis, msg, cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2);
            cv::putText(vis, "L-click: save this point | R-click: undo last", cv::Point(10, 60),
                        cv::FONT_HERSHEY_SIMPLEX, 0.65, cv::Scalar(0, 255, 255), 2);
        }
        else
        {
            cv::putText(vis, "DONE: all points saved. (R-click to undo) ", cv::Point(10, 30),
                        cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 0), 2);
        }

        // 이미 저장된 포인트 마커 표시
        for (const auto& p : st.pts)
        {
            if (!p.done) continue;
            cv::circle(vis, cv::Point((int)p.u_px, (int)p.v_px), 5, cv::Scalar(0, 0, 255), -1, cv::LINE_AA);
        }

        // 저장 경로 표시
        cv::putText(vis, "out: " + out_path, cv::Point(10, vis.rows - 15),
                    cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(255, 255, 255), 1);

        cv::imshow(win, vis);
        int key = cv::waitKey(1) & 0xFF;
        if (key == 27 || key == 'q')
            break;
    }

    return 0;
}

