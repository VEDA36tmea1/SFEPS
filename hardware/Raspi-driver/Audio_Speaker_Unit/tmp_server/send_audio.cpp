#include <iostream>
#include <fstream>
#include <vector>
#include <string>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

// 간단한 TCP 클라이언트: RAW PCM 파일을 라즈베리 파이로 전송
// 포맷: 16kHz, mono, S16_LE (Int16)

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <port> <raw_file>\n";
        std::cerr << "  e.g. " << argv[0] << " 192.168.0.50 6000 test.raw\n";
        return 1;
    }

    std::string server_ip = argv[1];
    int port = std::stoi(argv[2]);
    std::string filename = argv[3];

    std::ifstream infile(filename, std::ios::binary);
    if (!infile) {
        std::cerr << "Failed to open file: " << filename << std::endl;
        return 1;
    }

    // 파일 전체를 메모리로 읽기 (테스트용: 파일 크기가 너무 크지 않다는 가정)
    std::vector<char> data((std::istreambuf_iterator<char>(infile)),
                           std::istreambuf_iterator<char>());
    infile.close();

    if (data.empty()) {
        std::cerr << "File is empty: " << filename << std::endl;
        return 1;
    }

    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::perror("socket");
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, server_ip.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "Invalid server IP: " << server_ip << std::endl;
        ::close(sock);
        return 1;
    }

    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("connect");
        ::close(sock);
        return 1;
    }

    std::cout << "[tmp_server] Sending " << data.size() << " bytes to "
              << server_ip << ":" << port << " ..." << std::endl;

    const char* p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        ssize_t sent = ::send(sock, p, left, 0);
        if (sent <= 0) {
            std::perror("send");
            break;
        }
        p    += sent;
        left -= sent;
    }

    ::close(sock);
    std::cout << "[tmp_server] Done.\n";
    return 0;
}

