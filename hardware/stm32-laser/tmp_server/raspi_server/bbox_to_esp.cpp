#include "bbox_to_esp.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace {

// Camera/get_metadata/inc/Config.h 와 동일 값 (4K 센서 기준)
constexpr float kSensorWidth  = 3840.0f;
constexpr float kSensorHeight = 2160.0f;

} // namespace

bool extractFirstHumanCenter(const std::string& xml, BBoxCenter& out_center) {
    size_t search_pos = 0;

    while (true) {
        size_t obj_start = xml.find("<tt:Object", search_pos);
        if (obj_start == std::string::npos) break;

        // 다음 Object 시작 지점 (범위 제한용)
        size_t next_obj = xml.find("<tt:Object", obj_start + 1);

        // 타입 추출
        std::string obj_type = "Unknown";
        size_t type_pos = xml.find("<tt:Type>", obj_start);
        if (type_pos != std::string::npos &&
            (next_obj == std::string::npos || type_pos < next_obj)) {
            size_t start = type_pos + 9;
            size_t end   = xml.find("</tt:Type>", start);
            if (end != std::string::npos) {
                obj_type = xml.substr(start, end - start);
            }
        }

        // Human 타입만 사용
        if (obj_type == "Human") {
            float x = -1.0f;
            float y = -1.0f;

            size_t x_pos = xml.find("x=\"", obj_start);
            size_t y_pos = xml.find("y=\"", obj_start);
            if (x_pos != std::string::npos && y_pos != std::string::npos &&
                (next_obj == std::string::npos || x_pos < next_obj)) {
                size_t end_x = xml.find("\"", x_pos + 3);
                size_t end_y = xml.find("\"", y_pos + 3);
                if (end_x != std::string::npos && end_y != std::string::npos) {
                    float raw_x = std::stof(xml.substr(x_pos + 3, end_x - (x_pos + 3)));
                    float raw_y = std::stof(xml.substr(y_pos + 3, end_y - (y_pos + 3)));

                    // XMLParser.cpp 와 동일한 정규화 규칙
                    if (raw_x > 1.0f) {
                        x = raw_x / kSensorWidth;
                        y = raw_y / kSensorHeight;
                    } else {
                        x = raw_x;
                        y = raw_y;
                    }
                }
            }

            if (x >= 0.0f && y >= 0.0f) {
                out_center.x = x;
                out_center.y = y;
                return true;
            }
        }

        search_pos = obj_start + 1;
    }

    return false;
}

bool sendCenterToEsp(const BBoxCenter& center,
                     const std::string& esp_ip,
                     unsigned short esp_port) {
    if (center.x < 0.0f || center.y < 0.0f) {
        return false;
    }

    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::perror("socket");
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(esp_port);
    if (::inet_pton(AF_INET, esp_ip.c_str(), &addr.sin_addr) <= 0) {
        std::fprintf(stderr, "Invalid ESP8266 IP: %s\n", esp_ip.c_str());
        ::close(sock);
        return false;
    }

    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("connect");
        ::close(sock);
        return false;
    }

    char buf[128];
    int len = std::snprintf(buf, sizeof(buf), "CX=%.6f,CY=%.6f\n", center.x, center.y);
    if (len <= 0 || len >= static_cast<int>(sizeof(buf))) {
        ::close(sock);
        return false;
    }

    const char* p    = buf;
    int         left = len;
    while (left > 0) {
        int sent = ::send(sock, p, left, 0);
        if (sent <= 0) {
            std::perror("send");
            ::close(sock);
            return false;
        }
        p    += sent;
        left -= sent;
    }

    ::close(sock);
    return true;
}

