#include <iostream>
#include <string>
#include <sys/socket.h>

#include "rtsp_client_simple.h"
#include "bbox_to_esp.h"

// Camera/get_metadata/src/main.cpp 를 참고한,
// 메타데이터 전용 테스트용 메인 함수.
// - RTSP over TCP (interleaved) 로 메타데이터 채널(2)만 읽어서
//   XML 누적 후, 프레임이 바뀔 때마다 첫 번째 Human 중심좌표를 출력.

int main() {
    RtspClientSimple client;

    if (!client.connectToCamera()) {
        std::cerr << "Camera connect failed." << std::endl;
        return 1;
    }

    client.sendHandshake();

    unsigned char header[4];
    char* big_buffer = new char[65536];
    std::string accumulated_xml;
    unsigned int last_timestamp = 0;

    int sock = client.getSocket();

    while (true) {
        client.sendHeartbeat();

        int read_len = ::recv(sock, header, 4, MSG_WAITALL);
        if (read_len <= 0) {
            std::cerr << "recv header failed or connection closed." << std::endl;
            break;
        }

        if (header[0] == '$') {
            int channel = static_cast<int>(header[1]);
            int payload_len = (static_cast<int>(header[2]) << 8) |
                               static_cast<int>(header[3]);

            int total_read = 0;
            while (total_read < payload_len) {
                int to_read = payload_len - total_read;
                if (to_read > 65536) to_read = 65536;
                int r = ::recv(sock, big_buffer + total_read, to_read, 0);
                if (r <= 0) {
                    std::cerr << "recv payload failed." << std::endl;
                    break;
                }
                total_read += r;
            }

            // 메타데이터 채널 = 2 (Camera/get_metadata 와 동일 가정)
            if (channel == 2 && payload_len > 12) {
                unsigned char* rtp_ptr = reinterpret_cast<unsigned char*>(big_buffer);
                unsigned int current_timestamp =
                    (rtp_ptr[4] << 24) | (rtp_ptr[5] << 16) |
                    (rtp_ptr[6] << 8)  | rtp_ptr[7];

                char* xml_data = big_buffer + 12;
                int xml_len    = total_read - 12;

                if (current_timestamp != last_timestamp && last_timestamp != 0) {
                    // 누적 XML에서 첫 번째 Human 중심 좌표 추출
                    BBoxCenter center;
                    if (extractFirstHumanCenter(accumulated_xml, center)) {
                        std::cout << "[META] Human center = ("
                                  << center.x << ", " << center.y
                                  << ") at RTP=" << last_timestamp << std::endl;
                    } else {
                        std::cout << "[META] No Human bbox found at RTP="
                                  << last_timestamp << std::endl;
                    }

                    accumulated_xml.clear();
                }

                accumulated_xml.append(xml_data, xml_len);
                last_timestamp = current_timestamp;
            }
        }
    }

    delete[] big_buffer;
    return 0;
}

