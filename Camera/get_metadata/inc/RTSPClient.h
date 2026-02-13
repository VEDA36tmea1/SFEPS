#pragma once
#include <string>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>

class RTSPClient {
private:
    int sock;
    std::string session_id;
    time_t last_heartbeat;

public:
    RTSPClient();
    ~RTSPClient();

    bool connectToCamera();
    void sendHandshake(); // OPTIONS ~ PLAY 까지 수행
    void sendHeartbeat(); // Keep-Alive 전송
    int getSocket() const { return sock; }
    std::string getSessionId() const { return session_id; }
};
