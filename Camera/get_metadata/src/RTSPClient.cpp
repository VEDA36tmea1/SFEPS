#include "RTSPClient.h"
#include "Config.h"
#include <iostream>
#include <cstring>

RTSPClient::RTSPClient() : sock(-1), last_heartbeat(0) {}

RTSPClient::~RTSPClient() {
    if (sock != -1) close(sock);
}

bool RTSPClient::connectToCamera() {
    sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(CAMERA_PORT);
    inet_pton(AF_INET, CAMERA_IP, &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "❌ Connection Failed" << std::endl;
        return false;
    }
    std::cout << "✅ Connected to Camera!" << std::endl;
    return true;
}

void RTSPClient::sendHandshake() {
    char buffer[1024] = {0};
    std::string msg;

    // 1. OPTIONS
    msg = "OPTIONS " + std::string(RTSP_URL) + " RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: MyClient\r\n\r\n";
    send(sock, msg.c_str(), msg.length(), 0);
    recv(sock, buffer, 1024, 0);

    // 2. DESCRIBE
    msg = "DESCRIBE " + std::string(RTSP_URL) + " RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\nUser-Agent: MyClient\r\n\r\n";
    send(sock, msg.c_str(), msg.length(), 0);
    recv(sock, buffer, 1024, 0);

    // 3. SETUP (Video)
    msg = "SETUP " + std::string(RTSP_URL) + "/trackID=v RTSP/1.0\r\nCSeq: 3\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nUser-Agent: MyClient\r\n\r\n";
    send(sock, msg.c_str(), msg.length(), 0);
    memset(buffer, 0, 1024);
    recv(sock, buffer, 1024, 0);

    // Session ID Parsing
    std::string resp(buffer);
    size_t session_pos = resp.find("Session: ");
    if (session_pos != std::string::npos) {
        size_t end_pos = resp.find_first_of(";\r\n", session_pos);
        session_id = resp.substr(session_pos + 9, end_pos - (session_pos + 9));
        std::cout << "✅ Session ID: " << session_id << std::endl;
    }

    // 4. SETUP (Metadata)
    msg = "SETUP " + std::string(RTSP_URL) + "/trackID=m RTSP/1.0\r\nCSeq: 4\r\nTransport: RTP/AVP/TCP;unicast;interleaved=2-3\r\nSession: " + session_id + "\r\nUser-Agent: MyClient\r\n\r\n";
    send(sock, msg.c_str(), msg.length(), 0);
    recv(sock, buffer, 1024, 0);

    // 5. PLAY
    msg = "PLAY " + std::string(RTSP_URL) + " RTSP/1.0\r\nCSeq: 5\r\nSession: " + session_id + "\r\nRange: npt=0.000-\r\nUser-Agent: MyClient\r\n\r\n";
    send(sock, msg.c_str(), msg.length(), 0);
    recv(sock, buffer, 1024, 0);
    
    std::cout << "🚀 Streaming Started!" << std::endl;
    last_heartbeat = time(NULL);
}

void RTSPClient::sendHeartbeat() {
    time_t now = time(NULL);
    if (now - last_heartbeat > 30) {
        std::string msg = "GET_PARAMETER " + std::string(RTSP_URL) + " RTSP/1.0\r\nCSeq: 99\r\nSession: " + session_id + "\r\nUser-Agent: MyClient\r\n\r\n";
        send(sock, msg.c_str(), msg.length(), 0);
        last_heartbeat = now;
    }
}
