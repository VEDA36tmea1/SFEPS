// dump_metadata_xml — ONVIF 메타데이터(XML) 원시 덤프 또는 오프라인 파싱
//
// 사용법:
//   1) 카메라에서 실시간으로 메타 XML만 stdout에 출력 (RTP 타임스탬프 단위)
//      dump_metadata_xml.exe
//      dump_metadata_xml.exe --max-frames 5
//   2) 저장해 둔 XML 파일만 XMLParser로 파싱 (Human/Head 등 타입·id·bbox 확인)
//      dump_metadata_xml.exe --file captured.xml
//      dump_metadata_xml.exe --file captured.xml --detect-all
//   3) 표준입력에서 XML 읽기
//      type captured.xml | dump_metadata_xml.exe --stdin
//      dump_metadata_xml.exe --stdin --detect-all < captured.xml

// Config.h 를 XMLParser.h 보다 먼저 넣지 말 것: LOG_THROTTLE 등 매크로가 클래스 멤버 이름을 깨뜨림.
#include "RTSPClient.h"
#include "XMLParser.h"
#include "Config.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#endif

static std::string read_all_stream(std::istream& in) {
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

static void print_parsed(const XMLParser& parser, const std::string& xml, bool detect_all) {
    auto objs = parser.parseHumanObjectsForAnalytics(xml, detect_all);
    std::cout << "---- parseHumanObjectsForAnalytics (detect_all=" << (detect_all ? "true" : "false")
              << ") count=" << objs.size() << " ----\n";
    for (const auto& o : objs) {
        std::cout << "  id=" << o.id << " type=" << o.type
                  << " cog=(" << o.x << "," << o.y << ")"
                  << " bbox LTRB=(" << o.left << "," << o.top << "," << o.right << "," << o.bottom << ")\n";
    }
}

static int run_file_mode(const std::string& path, bool detect_all) {
    std::ifstream ifs(path);
    if (!ifs) {
        std::cerr << "[dump_metadata_xml] cannot open file: " << path << "\n";
        return 1;
    }
    const std::string xml = read_all_stream(ifs);
    std::cout << "---- raw XML (" << xml.size() << " bytes) from " << path << " ----\n"
              << xml << "\n";
    XMLParser parser;
    print_parsed(parser, xml, detect_all);
    return 0;
}

static int run_stdin_mode(bool detect_all) {
    const std::string xml = read_all_stream(std::cin);
    std::cout << "---- raw XML (" << xml.size() << " bytes) from stdin ----\n"
              << xml << "\n";
    XMLParser parser;
    print_parsed(parser, xml, detect_all);
    return 0;
}

static int run_rtsp_mode(int max_frames) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif
    RTSPClient client;
    if (!client.connectToCamera()) return 1;
    client.sendHandshake();

    char* big_buffer = new char[65536];
    std::string accumulated_xml;
    unsigned int last_timestamp = 0;
    const socket_t sock = client.getSocket();
    int frames_out = 0;

    while (true) {
        unsigned char header[4];
        int read_len = recv(sock, reinterpret_cast<char*>(header), 4, MSG_WAITALL);
        if (read_len <= 0) break;
        client.sendHeartbeat();

        if (header[0] != '$') continue;

        int channel = static_cast<int>(header[1]);
        int payload_len = (static_cast<int>(header[2]) << 8) | static_cast<int>(header[3]);

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

        if (channel == 2) {
            unsigned char* rtp_ptr = reinterpret_cast<unsigned char*>(big_buffer);
            unsigned int current_timestamp =
                (rtp_ptr[4] << 24) | (rtp_ptr[5] << 16) | (rtp_ptr[6] << 8) | rtp_ptr[7];
            char* xml_data = big_buffer + 12;
            int xml_len = total_read - 12;

            if (current_timestamp != last_timestamp && last_timestamp != 0) {
                std::cout << "======== RTP frame dump ts=" << last_timestamp
                          << " xml_bytes=" << accumulated_xml.size() << " ========\n";
                std::cout << accumulated_xml << "\n";

                XMLParser parser;
                print_parsed(parser, accumulated_xml, true);

                accumulated_xml.clear();
                frames_out++;
                if (max_frames > 0 && frames_out >= max_frames) break;
            }
            accumulated_xml.append(xml_data, xml_len);
            last_timestamp = current_timestamp;
        }
    }

    delete[] big_buffer;
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

int main(int argc, char** argv) {
    bool detect_all = false;
    int max_frames = 0;
    std::string file_path;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--detect-all") detect_all = true;
        else if (a == "--stdin") {
            return run_stdin_mode(detect_all);
        } else if (a == "--file" && i + 1 < argc) {
            file_path = argv[++i];
        } else if (a == "--max-frames" && i + 1 < argc) {
            max_frames = std::atoi(argv[++i]);
        }
    }

    if (!file_path.empty()) return run_file_mode(file_path, detect_all);

    std::cerr << "[dump_metadata_xml] RTSP live dump from " << CAMERA_IP << " (metadata track)\n";
    std::cerr << "  Ctrl+C to stop. Options: --max-frames N   (stop after N complete XML frames)\n";
    std::cerr << "  Offline: --file path.xml [--detect-all]   |   --stdin [--detect-all]\n";
    return run_rtsp_mode(max_frames);
}
