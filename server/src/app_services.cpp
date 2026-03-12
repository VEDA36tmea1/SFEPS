#include "app_services.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analytics.h"
#include "alert.h"
#include "audio_common.h"
#include "audio_playback.h"
#include "audio_ring_buffer.h"
#include "auth.h"
#include "log.h"
#include "net_utils.h"
#include "tls_server.h"

namespace {

constexpr int AUTH_PORT = 5555;
constexpr int AUDIO_PORT = 5556;
constexpr int ALERT_PORT = 5557;
constexpr int POSITION_PORT = 5558;
constexpr std::size_t kMaxObjectIdBytes = 128;

std::string trim_copy(const std::string& s) {
    std::size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    std::size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string normalize_login_key(const std::string& user) {
    std::string normalized = trim_copy(user);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized;
}

bool snapshots_equal(const AnalyticsProcessor::ObjectPositionSnapshot& lhs,
                     const AnalyticsProcessor::ObjectPositionSnapshot& rhs) {
    return lhs.object_id == rhs.object_id &&
           lhs.left == rhs.left &&
           lhs.top == rhs.top &&
           lhs.right == rhs.right &&
           lhs.bottom == rhs.bottom &&
           lhs.x == rhs.x &&
           lhs.y == rhs.y &&
           lhs.tag_time == rhs.tag_time;
}

std::string format_pos_line(const AnalyticsProcessor::ObjectPositionSnapshot& snapshot) {
    char line[512];
    const int n = std::snprintf(
        line, sizeof(line),
        "POS|%s|L=%.1f|T=%.1f|R=%.1f|B=%.1f|X=%.1f|Y=%.1f|TAG=%s\n",
        snapshot.object_id.c_str(), snapshot.left, snapshot.top, snapshot.right, snapshot.bottom,
        snapshot.x, snapshot.y, snapshot.tag_time.c_str());
    if (n <= 0 || n >= static_cast<int>(sizeof(line))) return "";
    return std::string(line, static_cast<std::size_t>(n));
}

std::string format_pos_end_line(const std::string& object_id, const char* reason) {
    std::string line = "POS_END|" + object_id + "|REASON=" + (reason ? std::string(reason) : "UNKNOWN");
    line.push_back('\n');
    return line;
}

std::string normalize_object_id_token(const std::string& raw) {
    std::string object_id = trim_copy(raw);
    if (object_id.size() > kMaxObjectIdBytes) {
        object_id.resize(kMaxObjectIdBytes);
    }
    return object_id;
}

}  // namespace

void run_audio_receiver(std::atomic<bool>& running, const SecurityRuntimeOptions& sec_cfg) {
    constexpr std::size_t kBufferSize = 4096;
    const std::size_t ring_capacity_bytes =
        static_cast<std::size_t>(AUDIO_SAMPLE_RATE * AUDIO_FRAME_BYTES);

    AudioRingBuffer ring(ring_capacity_bytes);
    AudioPlayback playback(ring);
    if (!playback.start()) {
        std::cerr << "[Audio] Failed to start AudioPlayback" << std::endl;
        return;
    }

    int plain_server_fd = -1;
    if (sec_cfg.app_plaintext_enable) {
        plain_server_fd = create_listen_socket(AUDIO_PORT, "Audio", sec_cfg.app_bind_ip);
        if (plain_server_fd < 0) {
            ring.stop();
            playback.stop();
            running = false;
            return;
        }
        std::cout << "[main.cpp] [Audio] listening plaintext on port " << AUDIO_PORT << std::endl;
    }

    TlsServer tls_server;
    if (sec_cfg.app_tls_enable) {
        std::string tls_err;
        TlsServerConfig tls_cfg;
        tls_cfg.port = sec_cfg.audio_tls_port;
        tls_cfg.cert_file = sec_cfg.app_tls_cert_file;
        tls_cfg.key_file = sec_cfg.app_tls_key_file;
        tls_cfg.handshake_timeout_ms = sec_cfg.app_tls_handshake_timeout_ms;
        tls_cfg.bind_ip = sec_cfg.app_bind_ip;
        tls_cfg.tag = "AudioTLS";

        if (!init_tls_server(tls_server, tls_cfg, tls_err)) {
            std::cerr << "[main.cpp] [Audio] failed to start TLS listener: " << tls_err << std::endl;
            if (plain_server_fd >= 0) close(plain_server_fd);
            ring.stop();
            playback.stop();
            running = false;
            return;
        }

        std::cout << "[main.cpp] [Audio] listening TLS on port " << sec_cfg.audio_tls_port
                  << std::endl;
    }

    char buf[kBufferSize];
    while (running.load()) {
        std::vector<pollfd> pfds;
        if (plain_server_fd >= 0) pfds.push_back(pollfd {plain_server_fd, POLLIN, 0});
        if (tls_server.listen_fd >= 0) pfds.push_back(pollfd {tls_server.listen_fd, POLLIN, 0});

        if (pfds.empty()) break;

        const int poll_ret = poll(pfds.data(), pfds.size(), 1000);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Audio] poll() failed: " << std::strerror(errno) << std::endl;
            break;
        }
        if (poll_ret == 0) continue;

        for (const pollfd& pfd : pfds) {
            if ((pfd.revents & POLLIN) == 0) continue;

            if (pfd.fd == plain_server_fd) {
                sockaddr_in peer_addr {};
                socklen_t peer_len = sizeof(peer_addr);
                const int client_fd =
                    accept(plain_server_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
                if (client_fd < 0) {
                    if (!running.load()) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[Audio] accept() failed: " << std::strerror(errno) << std::endl;
                    continue;
                }

                const std::string client_ip = peer_ip_to_string(peer_addr);
                std::cout << "[main.cpp] [Audio] Plain connection attempt: ip=" << client_ip
                          << ", fd=" << client_fd << std::endl;
                if (!is_ip_allowed(sec_cfg.audio_allow_ips, client_ip)) {
                    std::cout << "[main.cpp] [Audio] Plain connection rejected by allowlist: ip="
                              << client_ip << std::endl;
                    close(client_fd);
                    continue;
                }

                apply_socket_read_timeout(client_fd, sec_cfg.socket_read_timeout_ms);

                bool oversize = false;
                std::size_t total_bytes = 0;
                while (running.load()) {
                    const ssize_t bytes_read = read(client_fd, buf, sizeof(buf));
                    if (bytes_read > 0) {
                        const std::size_t chunk = static_cast<std::size_t>(bytes_read);
                        if (total_bytes + chunk > sec_cfg.audio_max_bytes) {
                            oversize = true;
                            break;
                        }
                        ring.push(buf, chunk);
                        total_bytes += chunk;
                        continue;
                    }

                    if (bytes_read == 0) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[Audio] read() failed: " << std::strerror(errno) << std::endl;
                    break;
                }

                if (oversize) {
                    std::cout
                        << "[main.cpp] [Audio] Plain payload rejected: exceeded SFEPS_AUDIO_MAX_BYTES="
                        << sec_cfg.audio_max_bytes << " (ip=" << client_ip << ")" << std::endl;
                }

                close(client_fd);
                continue;
            }

            if (pfd.fd == tls_server.listen_fd) {
                sockaddr_in peer_addr {};
                TlsClientConnection client {};
                std::string tls_err;
                const int client_fd = accept_tls_client(tls_server, client, peer_addr, tls_err);
                if (client_fd < 0) {
                    if (!running.load()) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[main.cpp] [Audio] TLS accept failed: " << tls_err << std::endl;
                    continue;
                }

                const std::string client_ip = peer_ip_to_string(peer_addr);
                std::cout << "[main.cpp] [Audio] TLS connection attempt: ip=" << client_ip
                          << ", fd=" << client_fd << std::endl;
                if (!is_ip_allowed(sec_cfg.audio_allow_ips, client_ip)) {
                    std::cout << "[main.cpp] [Audio] TLS connection rejected by allowlist: ip="
                              << client_ip << std::endl;
                    close_tls_client(client);
                    continue;
                }

                apply_socket_read_timeout(client.fd, sec_cfg.socket_read_timeout_ms);

                bool oversize = false;
                std::size_t total_bytes = 0;
                while (running.load()) {
                    const ssize_t bytes_read = tls_read(client, buf, sizeof(buf));
                    if (bytes_read > 0) {
                        const std::size_t chunk = static_cast<std::size_t>(bytes_read);
                        if (total_bytes + chunk > sec_cfg.audio_max_bytes) {
                            oversize = true;
                            break;
                        }
                        ring.push(buf, chunk);
                        total_bytes += chunk;
                        continue;
                    }

                    if (bytes_read == 0) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[Audio] TLS read() failed: " << std::strerror(errno) << std::endl;
                    break;
                }

                if (oversize) {
                    std::cout
                        << "[main.cpp] [Audio] TLS payload rejected: exceeded SFEPS_AUDIO_MAX_BYTES="
                        << sec_cfg.audio_max_bytes << " (ip=" << client_ip << ")" << std::endl;
                }

                close_tls_client(client);
            }
        }
    }

    if (plain_server_fd >= 0) close(plain_server_fd);
    close_tls_server(tls_server);

    ring.stop();
    playback.stop();
    std::cout << "[main.cpp] [Audio] receiver thread stopped." << std::endl;
}

void run_fraud_notifier(std::atomic<bool>& running, const SecurityRuntimeOptions& sec_cfg) {
    int plain_server_fd = -1;
    if (sec_cfg.app_plaintext_enable) {
        plain_server_fd = create_listen_socket(ALERT_PORT, "Alert", sec_cfg.app_bind_ip);
        if (plain_server_fd < 0) {
            running = false;
            return;
        }
        std::cout << "[main.cpp] [Alert] listening plaintext on port " << ALERT_PORT << std::endl;
    }

    TlsServer tls_server;
    if (sec_cfg.app_tls_enable) {
        std::string tls_err;
        TlsServerConfig tls_cfg;
        tls_cfg.port = sec_cfg.alert_tls_port;
        tls_cfg.cert_file = sec_cfg.app_tls_cert_file;
        tls_cfg.key_file = sec_cfg.app_tls_key_file;
        tls_cfg.handshake_timeout_ms = sec_cfg.app_tls_handshake_timeout_ms;
        tls_cfg.bind_ip = sec_cfg.app_bind_ip;
        tls_cfg.tag = "AlertTLS";

        if (!init_tls_server(tls_server, tls_cfg, tls_err)) {
            std::cerr << "[main.cpp] [Alert] failed to start TLS listener: " << tls_err << std::endl;
            if (plain_server_fd >= 0) close(plain_server_fd);
            running = false;
            return;
        }

        std::cout << "[main.cpp] [Alert] listening TLS on port " << sec_cfg.alert_tls_port
                  << std::endl;
    }

    while (running.load()) {
        std::vector<pollfd> pfds;
        if (plain_server_fd >= 0) pfds.push_back(pollfd {plain_server_fd, POLLIN, 0});
        if (tls_server.listen_fd >= 0) pfds.push_back(pollfd {tls_server.listen_fd, POLLIN, 0});

        if (pfds.empty()) break;

        const int poll_ret = poll(pfds.data(), pfds.size(), 1000);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Alert] poll() failed: " << std::strerror(errno) << std::endl;
            break;
        }
        if (poll_ret == 0) continue;

        for (const pollfd& pfd : pfds) {
            if ((pfd.revents & POLLIN) == 0) continue;

            if (pfd.fd == plain_server_fd) {
                sockaddr_in peer_addr {};
                socklen_t peer_len = sizeof(peer_addr);
                const int client_fd =
                    accept(plain_server_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
                if (client_fd < 0) {
                    if (!running.load()) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[Alert] accept() failed: " << std::strerror(errno) << std::endl;
                    continue;
                }

                const std::string client_ip = peer_ip_to_string(peer_addr);
                std::cout << "[main.cpp] [Alert] Plain connection attempt: ip=" << client_ip
                          << ", fd=" << client_fd << std::endl;
                if (!is_ip_allowed(sec_cfg.alert_allow_ips, client_ip)) {
                    std::cout << "[main.cpp] [Alert] Plain connection rejected by allowlist: ip="
                              << client_ip << std::endl;
                    close(client_fd);
                    continue;
                }

                apply_socket_read_timeout(client_fd, sec_cfg.socket_read_timeout_ms);

                if (alert_client_count() >= sec_cfg.alert_max_clients) {
                    std::cout << "[main.cpp] [Alert] Plain connection rejected: max clients reached ("
                              << sec_cfg.alert_max_clients << ")" << std::endl;
                    close(client_fd);
                    continue;
                }

                add_alert_plain_client(client_fd);
                std::cout << "[main.cpp] [Alert] plain client connected: " << client_ip << ":"
                          << ntohs(peer_addr.sin_port) << " (fd=" << client_fd << ")" << std::endl;
                continue;
            }

            if (pfd.fd == tls_server.listen_fd) {
                sockaddr_in peer_addr {};
                TlsClientConnection client {};
                std::string tls_err;
                const int client_fd = accept_tls_client(tls_server, client, peer_addr, tls_err);
                if (client_fd < 0) {
                    if (!running.load()) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[main.cpp] [Alert] TLS accept failed: " << tls_err << std::endl;
                    continue;
                }

                const std::string client_ip = peer_ip_to_string(peer_addr);
                std::cout << "[main.cpp] [Alert] TLS connection attempt: ip=" << client_ip
                          << ", fd=" << client_fd << std::endl;
                if (!is_ip_allowed(sec_cfg.alert_allow_ips, client_ip)) {
                    std::cout << "[main.cpp] [Alert] TLS connection rejected by allowlist: ip="
                              << client_ip << std::endl;
                    close_tls_client(client);
                    continue;
                }

                apply_socket_read_timeout(client.fd, sec_cfg.socket_read_timeout_ms);

                if (alert_client_count() >= sec_cfg.alert_max_clients) {
                    std::cout << "[main.cpp] [Alert] TLS connection rejected: max clients reached ("
                              << sec_cfg.alert_max_clients << ")" << std::endl;
                    close_tls_client(client);
                    continue;
                }

                add_alert_tls_client(std::move(client));
                std::cout << "[main.cpp] [Alert] TLS client connected: " << client_ip << ":"
                          << ntohs(peer_addr.sin_port) << " (fd=" << client_fd << ")" << std::endl;
            }
        }
    }

    if (plain_server_fd >= 0) close(plain_server_fd);
    close_tls_server(tls_server);
    close_alert_client_connections();
    std::cout << "[main.cpp] [Alert] notifier thread stopped." << std::endl;
}

void run_position_stream_service(std::atomic<bool>& running,
                                 const SecurityRuntimeOptions& sec_cfg,
                                 AnalyticsProcessor& analytics) {
    struct PlainClientState {
        int fd = -1;
        std::string recv_buffer;
        std::string active_object_id;
        bool has_last_sent = false;
        AnalyticsProcessor::ObjectPositionSnapshot last_sent;
    };

    struct TlsClientState {
        TlsClientConnection conn {};
        std::string recv_buffer;
        std::string active_object_id;
        bool has_last_sent = false;
        AnalyticsProcessor::ObjectPositionSnapshot last_sent;
    };

    struct PollTarget {
        enum class Kind {
            PlainListen,
            TlsListen,
            PlainClient,
            TlsClient
        };
        Kind kind;
        std::size_t index;
    };

    int plain_server_fd = -1;
    if (sec_cfg.app_plaintext_enable) {
        plain_server_fd = create_listen_socket(POSITION_PORT, "Position", sec_cfg.app_bind_ip);
        if (plain_server_fd < 0) {
            running = false;
            return;
        }
        std::cout << "[main.cpp] [Position] listening plaintext on port " << POSITION_PORT
                  << std::endl;
    }

    TlsServer tls_server;
    if (sec_cfg.app_tls_enable) {
        std::string tls_err;
        TlsServerConfig tls_cfg;
        tls_cfg.port = sec_cfg.position_tls_port;
        tls_cfg.cert_file = sec_cfg.app_tls_cert_file;
        tls_cfg.key_file = sec_cfg.app_tls_key_file;
        tls_cfg.handshake_timeout_ms = sec_cfg.app_tls_handshake_timeout_ms;
        tls_cfg.bind_ip = sec_cfg.app_bind_ip;
        tls_cfg.tag = "PositionTLS";

        if (!init_tls_server(tls_server, tls_cfg, tls_err)) {
            std::cerr << "[main.cpp] [Position] failed to start TLS listener: " << tls_err
                      << std::endl;
            if (plain_server_fd >= 0) close(plain_server_fd);
            running = false;
            return;
        }

        std::cout << "[main.cpp] [Position] listening TLS on port " << sec_cfg.position_tls_port
                  << std::endl;
    }

    auto total_clients = [&](const std::vector<PlainClientState>& plain_clients,
                             const std::vector<TlsClientState>& tls_clients) -> std::size_t {
        return plain_clients.size() + tls_clients.size();
    };

    auto mark_remove = [](std::vector<std::size_t>& remove_indices, std::size_t index) {
        remove_indices.push_back(index);
    };

    auto unique_descending = [](std::vector<std::size_t>& indices) {
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        std::reverse(indices.begin(), indices.end());
    };

    std::vector<PlainClientState> plain_clients;
    std::vector<TlsClientState> tls_clients;
    constexpr std::size_t kMaxRecvBuffer = 16 * 1024;
    constexpr std::size_t kReadBufferSize = 4096;
    char read_buffer[kReadBufferSize];

    while (running.load()) {
        std::vector<pollfd> pfds;
        std::vector<PollTarget> targets;

        if (plain_server_fd >= 0) {
            pfds.push_back(pollfd {plain_server_fd, POLLIN, 0});
            targets.push_back(PollTarget {PollTarget::Kind::PlainListen, 0});
        }
        if (tls_server.listen_fd >= 0) {
            pfds.push_back(pollfd {tls_server.listen_fd, POLLIN, 0});
            targets.push_back(PollTarget {PollTarget::Kind::TlsListen, 0});
        }
        for (std::size_t i = 0; i < plain_clients.size(); ++i) {
            pfds.push_back(pollfd {plain_clients[i].fd, POLLIN | POLLHUP | POLLERR, 0});
            targets.push_back(PollTarget {PollTarget::Kind::PlainClient, i});
        }
        for (std::size_t i = 0; i < tls_clients.size(); ++i) {
            pfds.push_back(pollfd {tls_clients[i].conn.fd, POLLIN | POLLHUP | POLLERR, 0});
            targets.push_back(PollTarget {PollTarget::Kind::TlsClient, i});
        }

        const int tick_ms = std::max(1, sec_cfg.position_stream_tick_ms);
        const int poll_ret = poll(pfds.data(), pfds.size(), tick_ms);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Position] poll() failed: " << std::strerror(errno) << std::endl;
            break;
        }

        std::vector<std::size_t> remove_plain;
        std::vector<std::size_t> remove_tls;

        auto send_line_plain = [&](int fd, const std::string& line) -> bool {
            if (line.empty()) return true;
            return send_all_plain(fd, line.data(), line.size());
        };
        auto send_line_tls = [&](const TlsClientConnection& client, const std::string& line) -> bool {
            if (line.empty()) return true;
            return send_all_tls(client, line.data(), line.size());
        };

        if (poll_ret > 0) {
            for (std::size_t i = 0; i < pfds.size(); ++i) {
                const short revents = pfds[i].revents;
                if (revents == 0) continue;

                const PollTarget target = targets[i];

                if (target.kind == PollTarget::Kind::PlainListen) {
                    if ((revents & POLLIN) == 0) continue;
                    sockaddr_in peer_addr {};
                    socklen_t peer_len = sizeof(peer_addr);
                    const int client_fd =
                        accept(plain_server_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
                    if (client_fd < 0) {
                        if (!running.load()) break;
                        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                        std::cerr << "[Position] accept() failed: " << std::strerror(errno)
                                  << std::endl;
                        continue;
                    }

                    const std::string client_ip = peer_ip_to_string(peer_addr);
                    if (!is_ip_allowed(sec_cfg.alert_allow_ips, client_ip)) {
                        std::cout << "[main.cpp] [Position] Plain connection rejected by allowlist: ip="
                                  << client_ip << std::endl;
                        close(client_fd);
                        continue;
                    }

                    apply_socket_read_timeout(client_fd, sec_cfg.socket_read_timeout_ms);

                    if (total_clients(plain_clients, tls_clients) >= sec_cfg.position_max_clients) {
                        std::cout << "[main.cpp] [Position] Plain connection rejected: max clients reached ("
                                  << sec_cfg.position_max_clients << ")" << std::endl;
                        close(client_fd);
                        continue;
                    }

                    PlainClientState state;
                    state.fd = client_fd;
                    plain_clients.push_back(std::move(state));
                    std::cout << "[main.cpp] [Position] plain client connected: " << client_ip << ":"
                              << ntohs(peer_addr.sin_port) << " (fd=" << client_fd << ")"
                              << std::endl;
                    continue;
                }

                if (target.kind == PollTarget::Kind::TlsListen) {
                    if ((revents & POLLIN) == 0) continue;
                    sockaddr_in peer_addr {};
                    TlsClientConnection client {};
                    std::string tls_err;
                    const int client_fd = accept_tls_client(tls_server, client, peer_addr, tls_err);
                    if (client_fd < 0) {
                        if (!running.load()) break;
                        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                        std::cerr << "[main.cpp] [Position] TLS accept failed: " << tls_err
                                  << std::endl;
                        continue;
                    }

                    const std::string client_ip = peer_ip_to_string(peer_addr);
                    if (!is_ip_allowed(sec_cfg.alert_allow_ips, client_ip)) {
                        std::cout << "[main.cpp] [Position] TLS connection rejected by allowlist: ip="
                                  << client_ip << std::endl;
                        close_tls_client(client);
                        continue;
                    }

                    apply_socket_read_timeout(client.fd, sec_cfg.socket_read_timeout_ms);

                    if (total_clients(plain_clients, tls_clients) >= sec_cfg.position_max_clients) {
                        std::cout << "[main.cpp] [Position] TLS connection rejected: max clients reached ("
                                  << sec_cfg.position_max_clients << ")" << std::endl;
                        close_tls_client(client);
                        continue;
                    }

                    TlsClientState state;
                    state.conn = std::move(client);
                    tls_clients.push_back(std::move(state));
                    std::cout << "[main.cpp] [Position] TLS client connected: " << client_ip << ":"
                              << ntohs(peer_addr.sin_port) << " (fd=" << client_fd << ")"
                              << std::endl;
                    continue;
                }

                if (target.kind == PollTarget::Kind::PlainClient) {
                    if (target.index >= plain_clients.size()) continue;
                    auto& client = plain_clients[target.index];

                    if ((revents & (POLLHUP | POLLERR)) != 0) {
                        mark_remove(remove_plain, target.index);
                        continue;
                    }
                    if ((revents & POLLIN) == 0) continue;

                    const ssize_t bytes_read = read(client.fd, read_buffer, sizeof(read_buffer));
                    if (bytes_read == 0) {
                        mark_remove(remove_plain, target.index);
                        continue;
                    }
                    if (bytes_read < 0) {
                        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                        mark_remove(remove_plain, target.index);
                        continue;
                    }

                    client.recv_buffer.append(read_buffer, static_cast<std::size_t>(bytes_read));
                    while (true) {
                        const std::size_t nl = client.recv_buffer.find('\n');
                        if (nl == std::string::npos) break;
                        const std::string line = trim_copy(client.recv_buffer.substr(0, nl));
                        client.recv_buffer.erase(0, nl + 1);
                        if (line.empty()) continue;

                        if (line.rfind("SUB_POS|", 0) == 0) {
                            const std::string requested_id = normalize_object_id_token(line.substr(8));
                            if (requested_id.empty()) continue;

                            if (!client.active_object_id.empty() &&
                                client.active_object_id != requested_id) {
                                const std::string end_line =
                                    format_pos_end_line(client.active_object_id, "SWITCH");
                                if (!send_line_plain(client.fd, end_line)) {
                                    mark_remove(remove_plain, target.index);
                                    break;
                                }
                            }

                            client.active_object_id = requested_id;
                            client.has_last_sent = false;
                            continue;
                        }

                        if (line.rfind("UNSUB_POS|", 0) == 0) {
                            const std::string requested_id = normalize_object_id_token(line.substr(10));
                            if (requested_id.empty()) continue;
                            if (client.active_object_id == requested_id) {
                                const std::string end_line =
                                    format_pos_end_line(client.active_object_id, "UNSUB");
                                if (!send_line_plain(client.fd, end_line)) {
                                    mark_remove(remove_plain, target.index);
                                    break;
                                }
                                client.active_object_id.clear();
                                client.has_last_sent = false;
                            }
                            continue;
                        }
                    }

                    if (client.recv_buffer.size() > kMaxRecvBuffer) {
                        client.recv_buffer.clear();
                    }
                    continue;
                }

                if (target.kind == PollTarget::Kind::TlsClient) {
                    if (target.index >= tls_clients.size()) continue;
                    auto& client = tls_clients[target.index];

                    if ((revents & (POLLHUP | POLLERR)) != 0) {
                        mark_remove(remove_tls, target.index);
                        continue;
                    }
                    if ((revents & POLLIN) == 0) continue;

                    const ssize_t bytes_read =
                        tls_read(client.conn, read_buffer, sizeof(read_buffer));
                    if (bytes_read == 0) {
                        mark_remove(remove_tls, target.index);
                        continue;
                    }
                    if (bytes_read < 0) {
                        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                        mark_remove(remove_tls, target.index);
                        continue;
                    }

                    client.recv_buffer.append(read_buffer, static_cast<std::size_t>(bytes_read));
                    while (true) {
                        const std::size_t nl = client.recv_buffer.find('\n');
                        if (nl == std::string::npos) break;
                        const std::string line = trim_copy(client.recv_buffer.substr(0, nl));
                        client.recv_buffer.erase(0, nl + 1);
                        if (line.empty()) continue;

                        if (line.rfind("SUB_POS|", 0) == 0) {
                            const std::string requested_id = normalize_object_id_token(line.substr(8));
                            if (requested_id.empty()) continue;

                            if (!client.active_object_id.empty() &&
                                client.active_object_id != requested_id) {
                                const std::string end_line =
                                    format_pos_end_line(client.active_object_id, "SWITCH");
                                if (!send_line_tls(client.conn, end_line)) {
                                    mark_remove(remove_tls, target.index);
                                    break;
                                }
                            }

                            client.active_object_id = requested_id;
                            client.has_last_sent = false;
                            continue;
                        }

                        if (line.rfind("UNSUB_POS|", 0) == 0) {
                            const std::string requested_id = normalize_object_id_token(line.substr(10));
                            if (requested_id.empty()) continue;
                            if (client.active_object_id == requested_id) {
                                const std::string end_line =
                                    format_pos_end_line(client.active_object_id, "UNSUB");
                                if (!send_line_tls(client.conn, end_line)) {
                                    mark_remove(remove_tls, target.index);
                                    break;
                                }
                                client.active_object_id.clear();
                                client.has_last_sent = false;
                            }
                            continue;
                        }
                    }

                    if (client.recv_buffer.size() > kMaxRecvBuffer) {
                        client.recv_buffer.clear();
                    }
                }
            }
        }

        const auto erase_removed_clients = [&]() {
            unique_descending(remove_plain);
            for (const std::size_t idx : remove_plain) {
                if (idx >= plain_clients.size()) continue;
                if (plain_clients[idx].fd >= 0) close(plain_clients[idx].fd);
                plain_clients.erase(plain_clients.begin() + static_cast<std::ptrdiff_t>(idx));
            }
            remove_plain.clear();

            unique_descending(remove_tls);
            for (const std::size_t idx : remove_tls) {
                if (idx >= tls_clients.size()) continue;
                close_tls_client(tls_clients[idx].conn);
                tls_clients.erase(tls_clients.begin() + static_cast<std::ptrdiff_t>(idx));
            }
            remove_tls.clear();
        };

        erase_removed_clients();

        const auto now = std::chrono::steady_clock::now();
        const auto stale_limit =
            std::chrono::seconds(static_cast<long long>(sec_cfg.position_stale_seconds));

        for (std::size_t idx = 0; idx < plain_clients.size(); ++idx) {
            auto& client = plain_clients[idx];
            if (client.active_object_id.empty()) continue;

            AnalyticsProcessor::ObjectPositionSnapshot snapshot;
            const bool has_snapshot =
                analytics.getObjectPositionSnapshot(client.active_object_id, snapshot);
            const bool is_stale =
                (!has_snapshot) || ((now - snapshot.updated_at) > stale_limit);

            if (is_stale) {
                const std::string end_line =
                    format_pos_end_line(client.active_object_id, "STALE");
                if (!send_line_plain(client.fd, end_line)) {
                    mark_remove(remove_plain, idx);
                    continue;
                }
                client.active_object_id.clear();
                client.has_last_sent = false;
                continue;
            }

            if (client.has_last_sent && snapshots_equal(client.last_sent, snapshot)) {
                continue;
            }

            const std::string pos_line = format_pos_line(snapshot);
            if (!send_line_plain(client.fd, pos_line)) {
                mark_remove(remove_plain, idx);
                continue;
            }
            client.last_sent = snapshot;
            client.has_last_sent = true;
        }

        for (std::size_t idx = 0; idx < tls_clients.size(); ++idx) {
            auto& client = tls_clients[idx];
            if (client.active_object_id.empty()) continue;

            AnalyticsProcessor::ObjectPositionSnapshot snapshot;
            const bool has_snapshot =
                analytics.getObjectPositionSnapshot(client.active_object_id, snapshot);
            const bool is_stale =
                (!has_snapshot) || ((now - snapshot.updated_at) > stale_limit);

            if (is_stale) {
                const std::string end_line =
                    format_pos_end_line(client.active_object_id, "STALE");
                if (!send_line_tls(client.conn, end_line)) {
                    mark_remove(remove_tls, idx);
                    continue;
                }
                client.active_object_id.clear();
                client.has_last_sent = false;
                continue;
            }

            if (client.has_last_sent && snapshots_equal(client.last_sent, snapshot)) {
                continue;
            }

            const std::string pos_line = format_pos_line(snapshot);
            if (!send_line_tls(client.conn, pos_line)) {
                mark_remove(remove_tls, idx);
                continue;
            }
            client.last_sent = snapshot;
            client.has_last_sent = true;
        }

        erase_removed_clients();
    }

    for (auto& client : plain_clients) {
        if (client.fd >= 0) close(client.fd);
    }
    for (auto& client : tls_clients) {
        close_tls_client(client.conn);
    }
    if (plain_server_fd >= 0) close(plain_server_fd);
    close_tls_server(tls_server);
    std::cout << "[main.cpp] [Position] stream service thread stopped." << std::endl;
}

void run_login_auth(std::atomic<bool>& running,
                    const RuntimeConfig& cfg,
                    const SecurityRuntimeOptions& sec_cfg) {
    struct AttemptState {
        int fail_count = 0;
        std::chrono::steady_clock::time_point lock_until =
            std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point last_seen =
            std::chrono::steady_clock::time_point::min();
    };

    constexpr int kMaxFail = 5;
    constexpr int kCleanupInterval = 100;
    const auto kLockDuration = std::chrono::seconds(30);
    const auto kStaleRetention = std::chrono::minutes(10);

    Authenticator auth(cfg.db_host.c_str(), cfg.db_user.c_str(), cfg.db_pass.c_str(),
                       cfg.db_name_analytics.c_str());
    if (!auth.connect()) {
        std::cerr << "[main.cpp] [Fatal] Auth DB connection failed (fail-closed)." << std::endl;
        running = false;
        return;
    }

    DBLogger auth_logger(cfg.db_host.c_str(), cfg.db_user.c_str(), cfg.db_pass.c_str(),
                         cfg.db_name_analytics.c_str());
    if (!auth_logger.connect()) {
        std::cerr << "[main.cpp] [Warn] Auth logger DB connection failed. "
                  << "Login service will continue without auth DB log writes." << std::endl;
    }

    std::cout << "[AuthFlow][4] run_login_auth started. "
              << "plaintext=" << (sec_cfg.app_plaintext_enable ? "on" : "off")
              << ", tls=" << (sec_cfg.app_tls_enable ? "on" : "off")
              << ", auth_tls_port=" << sec_cfg.auth_tls_port << std::endl;

    int plain_server_fd = -1;
    if (sec_cfg.app_plaintext_enable) {
        plain_server_fd = create_listen_socket(AUTH_PORT, "Auth", sec_cfg.app_bind_ip);
        if (plain_server_fd < 0) {
            running = false;
            return;
        }
        std::cout << "[main.cpp] [Auth] listening plaintext on port " << AUTH_PORT << std::endl;
        std::cout << "[AuthFlow][4] auth plaintext listener ready on " << AUTH_PORT << std::endl;
    }

    TlsServer tls_server;
    if (sec_cfg.app_tls_enable) {
        std::string tls_err;
        TlsServerConfig tls_cfg;
        tls_cfg.port = sec_cfg.auth_tls_port;
        tls_cfg.cert_file = sec_cfg.app_tls_cert_file;
        tls_cfg.key_file = sec_cfg.app_tls_key_file;
        tls_cfg.handshake_timeout_ms = sec_cfg.app_tls_handshake_timeout_ms;
        tls_cfg.bind_ip = sec_cfg.app_bind_ip;
        tls_cfg.tag = "AuthTLS";

        if (!init_tls_server(tls_server, tls_cfg, tls_err)) {
            std::cerr << "[main.cpp] [Auth] failed to start TLS listener: " << tls_err << std::endl;
            if (plain_server_fd >= 0) close(plain_server_fd);
            running = false;
            return;
        }

        std::cout << "[main.cpp] [Auth] listening TLS on port " << sec_cfg.auth_tls_port
                  << std::endl;
        std::cout << "[AuthFlow][4] auth TLS listener ready on " << sec_cfg.auth_tls_port
                  << std::endl;
    }

    std::unordered_map<std::string, AttemptState> attempts;
    int request_counter = 0;

    auto evaluate_auth = [&](const std::string& data,
                             const std::string& client_ip,
                             bool oversized,
                             std::string& user,
                             bool& success) {
        user = "Unknown";
        success = false;
        bool valid_format = false;

        if (!oversized) {
            const std::size_t sep = data.find(':');
            if (sep != std::string::npos) {
                user = trim_copy(data.substr(0, sep));
                const std::string pass = trim_copy(data.substr(sep + 1));
                if (!user.empty() && !pass.empty()) {
                    valid_format = true;
                    const std::string login_key = normalize_login_key(user) + "|" + client_ip;
                    const auto now = std::chrono::steady_clock::now();
                    AttemptState& state = attempts[login_key];
                    state.last_seen = now;

                    if (state.lock_until > now) {
                        success = false;
                    } else {
                        success = auth.authenticate(user, pass);
                        if (success) {
                            state.fail_count = 0;
                            state.lock_until = std::chrono::steady_clock::time_point::min();
                        } else {
                            state.fail_count += 1;
                            if (state.fail_count >= kMaxFail) {
                                state.fail_count = 0;
                                state.lock_until = now + kLockDuration;
                            }
                        }
                    }
                }
            }
        }

        if (oversized) {
            success = false;
        } else if (!valid_format) {
            success = false;
        }

        if (++request_counter % kCleanupInterval == 0) {
            const auto now = std::chrono::steady_clock::now();
            for (auto it = attempts.begin(); it != attempts.end();) {
                const bool is_locked = it->second.lock_until > now;
                const bool is_stale =
                    it->second.last_seen != std::chrono::steady_clock::time_point::min() &&
                    ((now - it->second.last_seen) > kStaleRetention);
                if (!is_locked && it->second.fail_count == 0 && is_stale) {
                    it = attempts.erase(it);
                } else {
                    ++it;
                }
            }
        }
    };

    while (running.load()) {
        std::vector<pollfd> pfds;
        if (plain_server_fd >= 0) pfds.push_back(pollfd {plain_server_fd, POLLIN, 0});
        if (tls_server.listen_fd >= 0) pfds.push_back(pollfd {tls_server.listen_fd, POLLIN, 0});

        if (pfds.empty()) break;

        const int poll_ret = poll(pfds.data(), pfds.size(), 1000);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Auth] poll() failed: " << std::strerror(errno) << std::endl;
            break;
        }
        if (poll_ret == 0) continue;

        for (const pollfd& pfd : pfds) {
            if ((pfd.revents & POLLIN) == 0) continue;

            if (pfd.fd == plain_server_fd) {
                sockaddr_in peer_addr {};
                socklen_t peer_len = sizeof(peer_addr);
                const int client_fd =
                    accept(plain_server_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
                if (client_fd < 0) {
                    if (!running.load()) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[Auth] accept() failed: " << std::strerror(errno) << std::endl;
                    continue;
                }

                const std::string client_ip = peer_ip_to_string(peer_addr);
                std::cout << "[AuthFlow][4] plain auth client accepted: ip=" << client_ip
                          << ", fd=" << client_fd << std::endl;
                if (!is_ip_allowed(sec_cfg.auth_allow_ips, client_ip)) {
                    std::cout << "[main.cpp] [Auth] Plain connection rejected by allowlist: ip="
                              << client_ip << std::endl;
                    close(client_fd);
                    continue;
                }

                apply_socket_read_timeout(client_fd, sec_cfg.socket_read_timeout_ms);

                std::vector<char> buf(sec_cfg.auth_max_bytes + 1, 0);
                const ssize_t bytes_read = read(client_fd, buf.data(), buf.size());
                if (bytes_read <= 0) {
                    if (bytes_read < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                        std::cerr << "[Auth] read() failed: " << std::strerror(errno) << std::endl;
                    }
                    close(client_fd);
                    continue;
                }

                const bool oversized = static_cast<std::size_t>(bytes_read) > sec_cfg.auth_max_bytes;
                const std::size_t data_len =
                    oversized ? sec_cfg.auth_max_bytes : static_cast<std::size_t>(bytes_read);
                const std::string data(buf.data(), data_len);

                std::string user;
                bool success = false;
                evaluate_auth(data, client_ip, oversized, user, success);
                std::cout << "[main.cpp] [Auth] Plain login attempt result: ip=" << client_ip
                          << ", user=" << user << ", result=" << (success ? "PASS" : "FAIL")
                          << std::endl;

                if (oversized) {
                    std::cout
                        << "[main.cpp] [Auth] Plain payload rejected: exceeded SFEPS_AUTH_MAX_BYTES="
                        << sec_cfg.auth_max_bytes << " (ip=" << client_ip << ")" << std::endl;
                }

                const char* resp = success ? "PASS" : "FAIL";
                send_all_plain(client_fd, resp, 4);

                if (success) {
                    send_alert_to_clients("TEST|LOGIN_OK|" + user + "\n");
                }
                auth_logger.enqueueLogin(user, client_ip, success);

                close(client_fd);
                continue;
            }

            if (pfd.fd == tls_server.listen_fd) {
                sockaddr_in peer_addr {};
                TlsClientConnection client {};
                std::string tls_err;
                const int client_fd = accept_tls_client(tls_server, client, peer_addr, tls_err);
                if (client_fd < 0) {
                    if (!running.load()) break;
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    std::cerr << "[main.cpp] [Auth] TLS accept failed: " << tls_err << std::endl;
                    continue;
                }

                const std::string client_ip = peer_ip_to_string(peer_addr);
                std::cout << "[AuthFlow][4] TLS auth client accepted: ip=" << client_ip
                          << ", fd=" << client_fd << std::endl;
                if (!is_ip_allowed(sec_cfg.auth_allow_ips, client_ip)) {
                    std::cout << "[main.cpp] [Auth] TLS connection rejected by allowlist: ip="
                              << client_ip << std::endl;
                    close_tls_client(client);
                    continue;
                }

                apply_socket_read_timeout(client.fd, sec_cfg.socket_read_timeout_ms);

                std::vector<char> buf(sec_cfg.auth_max_bytes + 1, 0);
                const ssize_t bytes_read = tls_read(client, buf.data(), buf.size());
                if (bytes_read <= 0) {
                    if (bytes_read < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                        std::cerr << "[Auth] TLS read() failed: " << std::strerror(errno) << std::endl;
                    }
                    close_tls_client(client);
                    continue;
                }

                const bool oversized = static_cast<std::size_t>(bytes_read) > sec_cfg.auth_max_bytes;
                const std::size_t data_len =
                    oversized ? sec_cfg.auth_max_bytes : static_cast<std::size_t>(bytes_read);
                const std::string data(buf.data(), data_len);

                std::string user;
                bool success = false;
                evaluate_auth(data, client_ip, oversized, user, success);
                std::cout << "[main.cpp] [Auth] TLS login attempt result: ip=" << client_ip
                          << ", user=" << user << ", result=" << (success ? "PASS" : "FAIL")
                          << std::endl;

                if (oversized) {
                    std::cout
                        << "[main.cpp] [Auth] TLS payload rejected: exceeded SFEPS_AUTH_MAX_BYTES="
                        << sec_cfg.auth_max_bytes << " (ip=" << client_ip << ")" << std::endl;
                }

                const char* resp = success ? "PASS" : "FAIL";
                send_all_tls(client, resp, 4);

                if (success) {
                    send_alert_to_clients("TEST|LOGIN_OK|" + user + "\n");
                }
                auth_logger.enqueueLogin(user, client_ip, success);

                close_tls_client(client);
            }
        }
    }

    if (plain_server_fd >= 0) close(plain_server_fd);
    close_tls_server(tls_server);

    std::cout << "[main.cpp] [Auth] auth thread stopped." << std::endl;
}
