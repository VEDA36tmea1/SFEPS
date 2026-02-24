#include <iostream>
#include <atomic>
#include <csignal>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstdio>
#include <string>

#include "audio_common.h"
#include "audio_ring_buffer.h"
#include "audio_playback.h"

static std::atomic<bool> g_running(true);

void signal_handler(int)
{
    g_running = false;
}

// 간단한 테스트용 Receiver: TCP로 RAW PCM을 받아 링 버퍼로 push
// 포트: 6000 (필요 시 변경)
constexpr int AUDIO_TEST_PORT = 6000;

// RAW PCM 수신 → 링 버퍼 push (libasound Playback 스레드에서 재생)
void run_tcp_receiver_raw(AudioRingBuffer& ring)
{
    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::perror("socket");
        return;
    }

    int opt = 1;
    ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(AUDIO_TEST_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        ::close(server_fd);
        return;
    }

    if (::listen(server_fd, 5) < 0) {
        std::perror("listen");
        ::close(server_fd);
        return;
    }

    std::cout << "[Audio_Speaker_Unit] Listening on TCP port " << AUDIO_TEST_PORT << " ..." << std::endl;

    constexpr std::size_t BUF_SIZE = 4096;
    char buf[BUF_SIZE];

    while (g_running) {
        int client_fd = ::accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (!g_running) break;
            std::perror("accept");
            continue;
        }

        std::cout << "[Audio_Speaker_Unit] Client connected." << std::endl;

        ssize_t bytes;
        while (g_running && (bytes = ::read(client_fd, buf, BUF_SIZE)) > 0) {
            ring.push(buf, static_cast<std::size_t>(bytes));
        }

        ::close(client_fd);
        std::cout << "[Audio_Speaker_Unit] Client disconnected." << std::endl;
    }

    ring.stop();
    ::close(server_fd);
}

// MP3 수신 → mpg123 로 바로 재생 (링 버퍼/Playback 모듈 사용 안 함)
void run_tcp_receiver_mp3()
{
    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::perror("socket");
        return;
    }

    int opt = 1;
    ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(AUDIO_TEST_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        ::close(server_fd);
        return;
    }

    if (::listen(server_fd, 5) < 0) {
        std::perror("listen");
        ::close(server_fd);
        return;
    }

    std::cout << "[Audio_Speaker_Unit] (MP3 mode) Listening on TCP port " << AUDIO_TEST_PORT << " ..." << std::endl;

    constexpr std::size_t BUF_SIZE = 4096;
    char buf[BUF_SIZE];

    while (g_running) {
        int client_fd = ::accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (!g_running) break;
            std::perror("accept");
            continue;
        }

        std::cout << "[Audio_Speaker_Unit] (MP3 mode) Client connected." << std::endl;

        // mpg123 를 이용해 mp3 를 디코드 + 재생 (서버에 mpg123 설치 필요)
        FILE* mpg = ::popen("mpg123 -q -", "w");
        if (!mpg) {
            std::perror("popen mpg123");
            ::close(client_fd);
            continue;
        }

        ssize_t bytes;
        while (g_running && (bytes = ::read(client_fd, buf, BUF_SIZE)) > 0) {
            if (std::fwrite(buf, 1, static_cast<std::size_t>(bytes), mpg) != static_cast<std::size_t>(bytes)) {
                std::perror("fwrite to mpg123");
                break;
            }
        }

        ::pclose(mpg);
        ::close(client_fd);
        std::cout << "[Audio_Speaker_Unit] (MP3 mode) Client disconnected." << std::endl;
    }

    ::close(server_fd);
}

int main(int argc, char** argv)
{
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 포맷 선택: 기본 RAW, "--mp3" 옵션이 있으면 MP3 모드
    bool mp3_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--mp3") {
            mp3_mode = true;
        }
    }

    if (mp3_mode) {
        std::cout << "[Audio_Speaker_Unit] MP3 mode (mpg123) on port " << AUDIO_TEST_PORT << std::endl;
        run_tcp_receiver_mp3();
    } else {
        // 약 1초 정도 버퍼링할 수 있는 용량 예시: 16000 frames * 2 bytes * 1ch ≒ 32KB
        constexpr std::size_t RING_CAPACITY_BYTES = AUDIO_SAMPLE_RATE * AUDIO_FRAME_BYTES * 1;
        AudioRingBuffer ring(RING_CAPACITY_BYTES);
        AudioPlayback playback(ring);

        if (!playback.start()) {
            std::cerr << "Failed to start AudioPlayback" << std::endl;
            return 1;
        }

        std::cout << "[Audio_Speaker_Unit] RAW mode (16kHz, mono, S16_LE) on port " << AUDIO_TEST_PORT << std::endl;

        // Receiver: TCP -> ring buffer
        run_tcp_receiver_raw(ring);

        g_running = false;
        playback.stop();
    }

    std::cout << "[Audio_Speaker_Unit] Stopped." << std::endl;
    return 0;
}
