#pragma once
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using socket_t = SOCKET;
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
using socket_t = int;
#endif

#include <time.h>

class RTSPClient {
private:
    socket_t sock;
    std::string session_id;
    time_t last_heartbeat;

public:
    RTSPClient();
    ~RTSPClient();

    bool connectToCamera();
    void sendHandshake(); // OPTIONS ~ PLAY 까지 수행
    void sendHeartbeat(); // Keep-Alive 전송
    socket_t getSocket() const { return sock; }
    std::string getSessionId() const { return session_id; }
};
