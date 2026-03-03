#pragma once

#include <string>

// 카메라 메타데이터(XML)에서 추출한 객체 중심 좌표 (정규화: 0.0~1.0)
struct BBoxCenter {
    float x = -1.0f;
    float y = -1.0f;
};

// Camera/get_metadata 쪽 XMLParser.cpp 로직을 참고해,
// 첫 번째 Human 객체의 정규화 좌표를 추출한다.
// 성공 시 out_center에 채우고 true, 실패 시 false 반환.
bool extractFirstHumanCenter(const std::string& xml, BBoxCenter& out_center);

// ESP8266(TCP 서버 or 클라이언트)와 TCP로 연결해
// 중심 좌표를 간단한 텍스트 프로토콜로 전송한다.
// 포맷 예시: "CX=0.123456,CY=0.654321\n"
bool sendCenterToEsp(const BBoxCenter& center,
                     const std::string& esp_ip,
                     unsigned short esp_port);

