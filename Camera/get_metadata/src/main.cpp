#include <iostream>
#include <vector>
#include "RTSPClient.h"
#include "XMLParser.h"

int main() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return -1;
    }
#endif
    // 1. 객체 생성
    RTSPClient client;
    XMLParser parser;

    // 2. 연결 및 설정
    if (!client.connectToCamera()) {
        return -1;
    }
    client.sendHandshake();

    // 3. 데이터 수신 루프
    unsigned char header[4]; 
    char* big_buffer = new char[65536]; 
    std::string accumulated_xml = "";
    unsigned int last_timestamp = 0;

    socket_t sock = client.getSocket();

    while (true) {
        // Keep-Alive (30초마다)
        client.sendHeartbeat();

        // 헤더 읽기
        int read_len = recv(sock, reinterpret_cast<char*>(header), 4, MSG_WAITALL);
        if (read_len <= 0) break;

        if (header[0] == '$') {
            int channel = (int)header[1];
            int payload_len = ((int)header[2] << 8) | (int)header[3];

            // Payload 읽기
            int total_read = 0;
            while (total_read < payload_len) {
                int to_read = payload_len - total_read;
                if (to_read > 65536) to_read = 65536;
                int r = recv(sock, big_buffer + total_read, to_read, 0);
                if (r <= 0) break;
                total_read += r;
            }

            // XML 메타데이터 (Channel 2)
            if (channel == 2 && payload_len > 12) {
                unsigned char* rtp_ptr = (unsigned char*)big_buffer;
                unsigned int current_timestamp = (rtp_ptr[4] << 24) | (rtp_ptr[5] << 16) | (rtp_ptr[6] << 8) | rtp_ptr[7];
                char* xml_data = big_buffer + 12;
                int xml_len = total_read - 12;

                // 프레임이 바뀌었을 때 파싱 수행
                if (current_timestamp != last_timestamp && last_timestamp != 0) {
                    // 파싱 호출
                    parser.parseAndProcess(accumulated_xml, last_timestamp);
                    accumulated_xml = "";
                }
                accumulated_xml.append(xml_data, xml_len);
                last_timestamp = current_timestamp;
            }
        }
    }

    delete[] big_buffer;
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}