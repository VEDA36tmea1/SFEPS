#include <iostream>
#include <fstream>
#include <vector>
#include <string>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
const socket_t invalid_socket = INVALID_SOCKET;
#  define CLOSESOCK ::closesocket
#else
#  include <arpa/inet.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <unistd.h>
using socket_t = int;
const socket_t invalid_socket = -1;
#  define CLOSESOCK ::close
#endif

// 간단한 TCP 클라이언트: 임의의 바이너리(예: MP3, RAW PCM)를 라즈베리 파이로 전송
// (서버 쪽에서 MP3 디코딩 또는 RAW 재생을 담당한다고 가정)

int main(int argc, char** argv)
{
    int ret = 0;
    socket_t sock = invalid_socket;
    std::string server_ip;
    int port = 0;
    std::string filename;
    std::ifstream infile;
    std::vector<char> data;
    sockaddr_in addr{};
    const char* p = nullptr;
    std::size_t left = 0;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return 1;
    }
#endif

    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <port> <audio_file>\n";
        std::cerr << "  e.g. " << argv[0] << " 192.168.0.50 6000 music.mp3\n";
        ret = 1;
        goto cleanup;
    }

    server_ip = argv[1];
    port      = std::stoi(argv[2]);
    filename  = argv[3];

    infile.open(filename, std::ios::binary);
    if (!infile) {
        std::cerr << "Failed to open file: " << filename << std::endl;
        ret = 1;
        goto cleanup;
    }

    // 파일 전체를 메모리로 읽기 (테스트용: 파일 크기가 너무 크지 않다는 가정)
    data.assign((std::istreambuf_iterator<char>(infile)),
                std::istreambuf_iterator<char>());
    infile.close();

    if (data.empty()) {
        std::cerr << "File is empty: " << filename << std::endl;
        ret = 1;
        goto cleanup;
    }

    sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock == invalid_socket) {
        std::perror("socket");
        ret = 1;
        goto cleanup;
    }

    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, server_ip.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "Invalid server IP: " << server_ip << std::endl;
        ret = 1;
        goto cleanup;
    }

    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("connect");
        ret = 1;
        goto cleanup;
    }

    std::cout << "[tmp_server] Sending " << data.size() << " bytes to "
              << server_ip << ":" << port << " ..." << std::endl;

    p    = data.data();
    left = data.size();
    while (left > 0) {
        int sent = ::send(sock, p, static_cast<int>(left), 0);
        if (sent <= 0) {
            std::perror("send");
            ret = 1;
            break;
        }
        p    += sent;
        left -= static_cast<std::size_t>(sent);
    }

    if (ret == 0) {
        std::cout << "[tmp_server] Done.\n";
    }

cleanup:
    if (sock != invalid_socket) {
        CLOSESOCK(sock);
    }
#ifdef _WIN32
    WSACleanup();
#endif
    return ret;
}

