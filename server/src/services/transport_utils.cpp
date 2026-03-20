#include "transport_utils.h"

#include <arpa/inet.h>

#include <cerrno>
#include <cstring>
#include <iostream>

#include <sys/socket.h>
#include <unistd.h>

#include "net_utils.h"

namespace app_services_transport {

namespace {

void reset_client(AcceptedClient& client) {
    client.kind = TransportKind::Plain;
    client.fd = -1;
    client.port = 0;
    client.ip.clear();
    client.tls_conn = TlsClientConnection {};
}

}  // namespace

bool start_listener_bundle(ListenerBundle& bundle,
                           const SecurityRuntimeOptions& sec_cfg,
                           int plain_port,
                           int tls_port,
                           const char* service_tag,
                           const char* tls_tag,
                           bool plain_required,
                           bool tls_required) {
    bundle = ListenerBundle {};

    if (sec_cfg.app_plaintext_enable) {
        bundle.plain_server_fd = create_listen_socket(plain_port, service_tag, sec_cfg.app_bind_ip);
        if (bundle.plain_server_fd < 0) {
            if (plain_required) {
                close_listener_bundle(bundle);
                return false;
            }
            std::cerr << "[main.cpp] [" << service_tag << "] plaintext 리스너 비활성화."
                      << std::endl;
        } else {
            std::cout << "[main.cpp] [" << service_tag << "] plaintext 포트 리스닝 시작: "
                      << plain_port << std::endl;
        }
    }

    if (sec_cfg.app_tls_enable) {
        std::string tls_err;
        TlsServerConfig tls_cfg;
        tls_cfg.port = tls_port;
        tls_cfg.cert_file = sec_cfg.app_tls_cert_file;
        tls_cfg.key_file = sec_cfg.app_tls_key_file;
        tls_cfg.handshake_timeout_ms = sec_cfg.app_tls_handshake_timeout_ms;
        tls_cfg.bind_ip = sec_cfg.app_bind_ip;
        tls_cfg.tag = tls_tag;

        if (!init_tls_server(bundle.tls_server, tls_cfg, tls_err)) {
            std::cerr << "[main.cpp] [" << service_tag << "] TLS 리스너 시작 실패: "
                      << tls_err << std::endl;
            if (tls_required) {
                close_listener_bundle(bundle);
                return false;
            }
        } else {
            std::cout << "[main.cpp] [" << service_tag << "] TLS 포트 리스닝 시작: " << tls_port
                      << std::endl;
        }
    }

    if (bundle.plain_server_fd < 0 && bundle.tls_server.listen_fd < 0) {
        return false;
    }
    return true;
}

void close_listener_bundle(ListenerBundle& bundle) {
    if (bundle.plain_server_fd >= 0) {
        close(bundle.plain_server_fd);
        bundle.plain_server_fd = -1;
    }
    close_tls_server(bundle.tls_server);
}

void append_listener_pollfds(const ListenerBundle& bundle, std::vector<pollfd>& pfds) {
    if (bundle.plain_server_fd >= 0) {
        pfds.push_back(pollfd {bundle.plain_server_fd, POLLIN, 0});
    }
    if (bundle.tls_server.listen_fd >= 0) {
        pfds.push_back(pollfd {bundle.tls_server.listen_fd, POLLIN, 0});
    }
}

bool resolve_listener_kind(const ListenerBundle& bundle, int fd, TransportKind& kind) {
    if (fd == bundle.plain_server_fd) {
        kind = TransportKind::Plain;
        return true;
    }
    if (fd == bundle.tls_server.listen_fd) {
        kind = TransportKind::Tls;
        return true;
    }
    return false;
}

bool accept_client(const ListenerBundle& bundle,
                   TransportKind kind,
                   const std::unordered_set<std::string>& allowlist,
                   const char* service_tag,
                   AcceptedClient& out_client) {
    reset_client(out_client);

    if (kind == TransportKind::Plain) {
        sockaddr_in peer_addr {};
        socklen_t peer_len = sizeof(peer_addr);
        const int client_fd = accept(
            bundle.plain_server_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
        if (client_fd < 0) {
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "[" << service_tag << "] accept() 실패: " << std::strerror(errno)
                          << std::endl;
            }
            return false;
        }

        const std::string client_ip = peer_ip_to_string(peer_addr);
        std::cout << "[main.cpp] [" << service_tag << "] Plain 연결 시도: ip="
                  << client_ip << ", fd=" << client_fd << std::endl;
        if (!is_ip_allowed(allowlist, client_ip)) {
            std::cout << "[main.cpp] [" << service_tag
                      << "] Plain 연결 허용목록 거부: ip=" << client_ip
                      << std::endl;
            close(client_fd);
            return false;
        }

        out_client.kind = kind;
        out_client.fd = client_fd;
        out_client.ip = client_ip;
        out_client.port = ntohs(peer_addr.sin_port);
        return true;
    }

    sockaddr_in peer_addr {};
    TlsClientConnection tls_client {};
    std::string tls_err;
    const int client_fd = accept_tls_client(bundle.tls_server, tls_client, peer_addr, tls_err);
    if (client_fd < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            std::cerr << "[main.cpp] [" << service_tag << "] TLS accept 실패: " << tls_err
                      << std::endl;
        }
        return false;
    }

    const std::string client_ip = peer_ip_to_string(peer_addr);
    std::cout << "[main.cpp] [" << service_tag << "] TLS 연결 시도: ip=" << client_ip
              << ", fd=" << client_fd << std::endl;
    if (!is_ip_allowed(allowlist, client_ip)) {
        std::cout << "[main.cpp] [" << service_tag << "] TLS 연결 허용목록 거부: ip="
                  << client_ip << std::endl;
        close_tls_client(tls_client);
        return false;
    }

    out_client.kind = kind;
    out_client.fd = client_fd;
    out_client.ip = client_ip;
    out_client.port = ntohs(peer_addr.sin_port);
    out_client.tls_conn = std::move(tls_client);
    return true;
}

void apply_read_timeout(const AcceptedClient& client, int timeout_ms) {
    if (client.fd >= 0) {
        apply_socket_read_timeout(client.fd, timeout_ms);
    }
}

ssize_t client_read(const AcceptedClient& client, void* buf, std::size_t len) {
    if (client.kind == TransportKind::Plain) {
        return read(client.fd, buf, len);
    }
    return tls_read(client.tls_conn, buf, len);
}

bool client_send_all(const AcceptedClient& client, const char* data, std::size_t len) {
    if (client.kind == TransportKind::Plain) {
        return send_all_plain(client.fd, data, len);
    }
    return send_all_tls(client.tls_conn, data, len);
}

bool client_send_line(const AcceptedClient& client, const std::string& line) {
    if (line.empty()) return true;
    return client_send_all(client, line.data(), line.size());
}

void close_client(AcceptedClient& client) {
    if (client.kind == TransportKind::Plain) {
        if (client.fd >= 0) {
            close(client.fd);
            client.fd = -1;
        }
        return;
    }

    close_tls_client(client.tls_conn);
    client.fd = -1;
}

const char* transport_name(TransportKind kind) {
    return kind == TransportKind::Plain ? "Plain" : "TLS";
}

}  // namespace app_services_transport
