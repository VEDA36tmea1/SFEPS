/*
 * RC522 UDS 데몬 (C++)
 *
 * 구조: Hardware Driver (이 데몬, C++) ↔ Unix Domain Socket ↔ Main Server
 *
 * - /dev/rc522 를 열고 카드 태깅 시 이벤트를 UDS로 연결된 메인 서버에 전송.
 * - 데몬은 UDS 서버 역할: 소켓 경로에 listen, 메인 서버가 connect 하면 NDJSON 한 줄씩 전송.
 * - 데몬화 지원 (--no-daemon 으로 포그라운드 실행 가능).
 *
 * 빌드: g++ -o rc522_uds_daemon rc522_uds_daemon.cpp -I../Kernel_Driver -std=c++17
 * 실행: sudo ./rc522_uds_daemon [--no-daemon] [--socket PATH] [--trailer N]
 */

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

extern "C" {
#include "rc522_ioctl.h"
}

namespace {

const char* DEV_RC522 = "/dev/rc522";
const int DEVICE_ID = 1;
const int DEFAULT_TRAILER = 11;
const size_t JSON_TEXT_MAX = 48;
const char* DEFAULT_SOCKET_PATH = "/tmp/rc522_events.sock";

volatile sig_atomic_t g_running = 1;
int g_trailer = DEFAULT_TRAILER;
std::string g_socket_path = DEFAULT_SOCKET_PATH;
bool g_no_daemon = false;

void sig_handler(int) { g_running = 0; }

void install_signal_handlers() {
  struct sigaction sa {};
  sa.sa_handler = sig_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
}

void daemonize() {
  if (fork() != 0) _exit(0);
  if (setsid() < 0) _exit(1);
  if (fork() != 0) _exit(0);
  chdir("/");
  umask(0);
  for (int i = 0; i < 3; i++) close(i);
  open("/dev/null", O_RDONLY);
  open("/dev/null", O_WRONLY);
  dup(1);
}

void escape_json_string(const char* text, std::string& out) {
  out.clear();
  for (const char* p = text; *p; ++p) {
    char c = *p;
    if (c == '"' || c == '\\')
      out += '\\', out += c;
    else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
      out += buf;
    } else
      out += c;
  }
}

std::string format_tag_event(uint32_t uid, const char* text, time_t ts) {
  std::string escaped;
  escape_json_string(text, escaped);
  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"device_id\":%d,\"id\":\"%08X\",\"text\":\"%s\",\"timestamp\":%ld}\n",
           DEVICE_ID, static_cast<unsigned>(uid), escaped.c_str(), static_cast<long>(ts));
  return buf;
}

int create_uds_server(const std::string& path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  unlink(path.c_str());
  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, 1) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--no-daemon") == 0)
      g_no_daemon = true;
    else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
      g_socket_path = argv[++i];
    else if (strcmp(argv[i], "--trailer") == 0 && i + 1 < argc) {
      g_trailer = atoi(argv[++i]);
      if (g_trailer < 0 || (g_trailer + 1) % 4 != 0) {
        fprintf(stderr, "trailer는 11, 15, 19, ... (4의 배수-1) 이어야 함\n");
        return 1;
      }
    }
  }

  if (!g_no_daemon)
    daemonize();

  install_signal_handlers();

  int rc522_fd = open(DEV_RC522, O_RDWR);
  if (rc522_fd < 0) {
    perror("open DEV_RC522");
    return 1;
  }

  int listen_fd = create_uds_server(g_socket_path);
  if (listen_fd < 0) {
    perror("UDS bind/listen");
    close(rc522_fd);
    return 1;
  }

  while (g_running) {
    int client_fd = accept(listen_fd, nullptr, nullptr);
    if (client_fd < 0) {
      if (!g_running) break;
      if (errno != EINTR) perror("UDS accept");
      continue;
    }

    fprintf(stderr, "UDS: client connected (fd=%d)\n", client_fd);

    while (g_running) {
      uint32_t uid = 0;
      if (ioctl(rc522_fd, RC522_READ_CARD, &uid) < 0) {
        if (errno == EINTR) continue;
        fprintf(stderr, "[DEBUG] ioctl failed: errno=%d (%s)\n", errno, strerror(errno));
        usleep(100000);
        continue;
      }

      time_t ts = time(nullptr);
      struct rc522_read_text text_data {};
      text_data.trailer_block = g_trailer;
      if (ioctl(rc522_fd, RC522_READ_TEXT_SECTOR, &text_data) != 0) {
        text_data.uid = uid;
        text_data.text[0] = '\0';
      }

      std::string line = format_tag_event(text_data.uid, text_data.text, ts);
      ssize_t n = write(client_fd, line.data(), line.size());
      if (n <= 0 || static_cast<size_t>(n) != line.size()) {
        perror("UDS write");
        close(client_fd);
        client_fd = -1;
        break;
      }
      usleep(500000);
    }
    if (client_fd >= 0) close(client_fd);
  }

  close(listen_fd);
  unlink(g_socket_path.c_str());
  close(rc522_fd);
  return 0;
}
