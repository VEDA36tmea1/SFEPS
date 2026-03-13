
// camera_client.cpp
// - RTSPClient 로 ONVIF 메타데이터(XML) 수신
// - XMLParser 로 Human 바운딩 박스 파싱
// - OpenCV 로 영상 표시 + 박스 그리기
// - 마우스 클릭 시, 선택된 사람 바운딩 박스의 bottom-Y 기준 좌표를 stdout 으로 출력
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

#include <opencv2/opencv.hpp>

#include <atomic>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_detect_all{false};  // true 이면 Human 이외 타입도 모두 박스로 표시

static std::mutex g_obj_mutex;
static std::vector<ParsedMetadataObject> g_objects;  // 마지막 프레임 기준 Human 객체들

static cv::Mat g_last_frame;
static std::mutex g_frame_mutex;

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
    int sock = client->getSocket();

    while (g_running) {
        client->sendHeartbeat();

        int read_len = recv(sock, header, 4, MSG_WAITALL);
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
                    g_objects = std::move(humans);
                }
                accumulated_xml.clear();
            }
            accumulated_xml.append(xml_data, xml_len);
            last_timestamp = current_timestamp;
        }
    }

    delete[] big_buffer;
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
        // 좌표 스케일 판별:
        // - 값이 1.0 이하이면 [0,1] 정규화, W/H 곱
        // - 값이 1.0 초과이면 센서 픽셀 좌표(SENSOR_WIDTH/HEIGHT)라고 가정
        int left, right, top, bottom;
        if (std::max({obj.left, obj.right, obj.top, obj.bottom}) <= 1.5f) {
            left   = static_cast<int>(obj.left   * W);
            right  = static_cast<int>(obj.right  * W);
            top    = static_cast<int>(obj.top    * H);
            bottom = static_cast<int>(obj.bottom * H);
        } else {
            const double sx = static_cast<double>(W) / SENSOR_WIDTH;
            const double sy = static_cast<double>(H) / SENSOR_HEIGHT;
            left   = static_cast<int>(obj.left   * sx);
            right  = static_cast<int>(obj.right  * sx);
            top    = static_cast<int>(obj.top    * sy);
            bottom = static_cast<int>(obj.bottom * sy);
        }
        int width  = std::max(1, right - left);
        int height = std::max(1, bottom - top);

        cv::Rect rect(left, top, width, height);
        if (rect.contains(cv::Point(x, y))) {
            int bottom_y_px = rect.y + rect.height;
            int center_x_px = rect.x + rect.width / 2;

            // 시각 피드백
            cv::rectangle(frame_copy, rect, cv::Scalar(0, 255, 0), 2);
            cv::circle(frame_copy, cv::Point(center_x_px, bottom_y_px), 6,
                       cv::Scalar(0, 0, 255), -1);
            cv::imshow("camera_client", frame_copy);

            // stdout 으로 결과 출력 (ubuntu_tcp_server 에 파이프로 연결해서 사용 가능)
            std::cout << "HUMAN_BOTTOM "
                      << "id=" << obj.id
                      << " type=" << obj.type
                      << " center_x_px=" << center_x_px
                      << " bottom_y_px=" << bottom_y_px
                      << std::endl;
            std::fflush(stdout);
            break;
        }
    }
}

int main(int argc, char** argv)
{
    // 인자: --detect-all 이면 Human 외 타입도 모두 표시
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--detect-all")
            g_detect_all = true;
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
    cv::VideoCapture cap(RTSP_URL);
    if (!cap.isOpened()) {
        std::cerr << "[camera_client] RTSP 영상 열기 실패: " << RTSP_URL << std::endl;
        g_running = false;
        meta_thread.join();
        return -1;
    }

    cv::namedWindow("camera_client", cv::WINDOW_NORMAL);
    cv::setMouseCallback("camera_client", on_mouse, &g_last_frame);

    while (g_running) {
        cv::Mat frame;
        if (!cap.read(frame) || frame.empty()) {
            std::cerr << "[camera_client] 빈 프레임, 종료" << std::endl;
            break;
        }

        {
            std::lock_guard<std::mutex> lock(g_frame_mutex);
            g_last_frame = frame.clone();
        }

        int W = frame.cols;
        int H = frame.rows;

        // 현재 Human bounding box 들을 그려준다
        std::vector<ParsedMetadataObject> objs;
        {
            std::lock_guard<std::mutex> lock(g_obj_mutex);
            objs = g_objects;
        }
        for (const auto& obj : objs) {
            int left, right, top, bottom;
            if (std::max({obj.left, obj.right, obj.top, obj.bottom}) <= 1.5f) {
                left   = static_cast<int>(obj.left   * W);
                right  = static_cast<int>(obj.right  * W);
                top    = static_cast<int>(obj.top    * H);
                bottom = static_cast<int>(obj.bottom * H);
            } else {
                const double sx = static_cast<double>(W) / SENSOR_WIDTH;
                const double sy = static_cast<double>(H) / SENSOR_HEIGHT;
                left   = static_cast<int>(obj.left   * sx);
                right  = static_cast<int>(obj.right  * sx);
                top    = static_cast<int>(obj.top    * sy);
                bottom = static_cast<int>(obj.bottom * sy);
            }
            int width  = std::max(1, right - left);
            int height = std::max(1, bottom - top);

            cv::rectangle(frame, cv::Rect(left, top, width, height),
                          cv::Scalar(0, 255, 255), 2);
            cv::putText(frame, obj.id.c_str(), cv::Point(left, top - 5),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 255), 1);
        }

        cv::imshow("camera_client", frame);
        int key = cv::waitKey(1) & 0xFF;
        if (key == 27 || key == 'q') {
            g_running = false;
            break;
        }
    }

    g_running = false;
    if (meta_thread.joinable()) meta_thread.join();
    cap.release();
    cv::destroyAllWindows();
    return 0;
}
