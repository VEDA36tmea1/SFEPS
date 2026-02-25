#pragma once

#include <string>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>

// Camera/get_metadata/inc/RTSPClient.h 를 참고한,
// 메타데이터(채널 2)를 받기 위한 최소 RTSP 클라이언트
class RtspClientSimple {
private:
    int sock_;
    std::string session_id_;
    time_t last_heartbeat_;

public:
    RtspClientSimple();
    ~RtspClientSimple();

    bool connectToCamera();
    void sendHandshake();   // OPTIONS ~ PLAY
    void sendHeartbeat();   // Keep-Alive

    int getSocket() const { return sock_; }
    std::string getSessionId() const { return session_id_; }
};

