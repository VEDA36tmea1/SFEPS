#include "rtsp_client_simple.h"
#include "camera_config.h"

#include <iostream>
#include <cstring>

RtspClientSimple::RtspClientSimple()
    : sock_(-1), last_heartbeat_(0) {}

RtspClientSimple::~RtspClientSimple() {
    if (sock_ != -1) {
        ::close(sock_);
        sock_ = -1;
    }
}

bool RtspClientSimple::connectToCamera() {
    sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
        std::perror("socket");
        return false;
    }

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(CAMERA_PORT);
    if (::inet_pton(AF_INET, CAMERA_IP, &serv_addr.sin_addr) <= 0) {
        std::cerr << "Invalid CAMERA_IP: " << CAMERA_IP << std::endl;
        ::close(sock_);
        sock_ = -1;
        return false;
    }

    if (::connect(sock_, reinterpret_cast<sockaddr*>(&serv_addr), sizeof(serv_addr)) < 0) {
        std::perror("connect");
        ::close(sock_);
        sock_ = -1;
        return false;
    }

    std::cout << "✅ Connected to Camera (" << CAMERA_IP << ":" << CAMERA_PORT << ")" << std::endl;
    return true;
}

void RtspClientSimple::sendHandshake() {
    char buffer[1024] = {0};
    std::string msg;

    // 1. OPTIONS
    msg = "OPTIONS " + std::string(RTSP_URL) + " RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: SFEPSMetaClient\r\n\r\n";
    ::send(sock_, msg.c_str(), msg.length(), 0);
    ::recv(sock_, buffer, sizeof(buffer), 0);

    // 2. DESCRIBE
    msg = "DESCRIBE " + std::string(RTSP_URL) + " RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\nUser-Agent: SFEPSMetaClient\r\n\r\n";
    ::send(sock_, msg.c_str(), msg.length(), 0);
    ::recv(sock_, buffer, sizeof(buffer), 0);

    // 3. SETUP (Video, interleaved 0-1)
    msg = "SETUP " + std::string(RTSP_URL) + "/trackID=v RTSP/1.0\r\n"
          "CSeq: 3\r\n"
          "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
          "User-Agent: SFEPSMetaClient\r\n\r\n";
    ::send(sock_, msg.c_str(), msg.length(), 0);
    std::memset(buffer, 0, sizeof(buffer));
    ::recv(sock_, buffer, sizeof(buffer), 0);

    // Session ID 파싱
    std::string resp(buffer);
    size_t session_pos = resp.find("Session: ");
    if (session_pos != std::string::npos) {
        size_t end_pos = resp.find_first_of(";\r\n", session_pos);
        session_id_ = resp.substr(session_pos + 9, end_pos - (session_pos + 9));
        std::cout << "✅ Session ID: " << session_id_ << std::endl;
    }

    // 4. SETUP (Metadata, interleaved 2-3)
    msg = "SETUP " + std::string(RTSP_URL) + "/trackID=m RTSP/1.0\r\n"
          "CSeq: 4\r\n"
          "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n"
          "Session: " + session_id_ + "\r\n"
          "User-Agent: SFEPSMetaClient\r\n\r\n";
    ::send(sock_, msg.c_str(), msg.length(), 0);
    ::recv(sock_, buffer, sizeof(buffer), 0);

    // 5. PLAY
    msg = "PLAY " + std::string(RTSP_URL) + " RTSP/1.0\r\n"
          "CSeq: 5\r\n"
          "Session: " + session_id_ + "\r\n"
          "Range: npt=0.000-\r\n"
          "User-Agent: SFEPSMetaClient\r\n\r\n";
    ::send(sock_, msg.c_str(), msg.length(), 0);
    ::recv(sock_, buffer, sizeof(buffer), 0);

    std::cout << "🚀 Metadata streaming started (TCP interleaved 2-3)" << std::endl;
    last_heartbeat_ = ::time(nullptr);
}

void RtspClientSimple::sendHeartbeat() {
    time_t now = ::time(nullptr);
    if (now - last_heartbeat_ > 30 && !session_id_.empty()) {
        std::string msg = "GET_PARAMETER " + std::string(RTSP_URL) + " RTSP/1.0\r\n"
                          "CSeq: 99\r\n"
                          "Session: " + session_id_ + "\r\n"
                          "User-Agent: SFEPSMetaClient\r\n\r\n";
        ::send(sock_, msg.c_str(), msg.length(), 0);
        last_heartbeat_ = now;
    }
}

