#include "tls_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

bool set_fd_nonblocking(int fd, bool enable) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;

    int next = flags;
    if (enable) {
        next |= O_NONBLOCK;
    } else {
        next &= ~O_NONBLOCK;
    }

    if (fcntl(fd, F_SETFL, next) != 0) return false;
    return true;
}

int wait_ssl_io(int fd, int ssl_error, int timeout_ms) {
    short events = POLLIN;
    if (ssl_error == SSL_ERROR_WANT_WRITE) {
        events = POLLOUT;
    }

    pollfd pfd {fd, events, 0};
    return poll(&pfd, 1, timeout_ms);
}

void reset_client(TlsClientConnection& client) {
    client.fd = -1;
    client.ssl = nullptr;
}

}  // namespace

bool init_tls_server(TlsServer& server, const TlsServerConfig& cfg, std::string& err) {
    close_tls_server(server);

    if (cfg.port <= 0) {
        err = "invalid tls port";
        return false;
    }
    if (cfg.cert_file.empty() || cfg.key_file.empty()) {
        err = "tls cert/key path is empty";
        return false;
    }
    in_addr bind_addr {};
    if (inet_pton(AF_INET, cfg.bind_ip.c_str(), &bind_addr) != 1) {
        err = "invalid bind IP: " + cfg.bind_ip;
        return false;
    }

    SSL_load_error_strings();
    OPENSSL_init_ssl(0, nullptr);

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == nullptr) {
        err = "SSL_CTX_new failed";
        return false;
    }

    if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1) {
        err = "failed to enforce TLS >= 1.2";
        SSL_CTX_free(ctx);
        return false;
    }

    if (SSL_CTX_use_certificate_file(ctx, cfg.cert_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        err = "failed to load tls certificate: " + cfg.cert_file;
        SSL_CTX_free(ctx);
        return false;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, cfg.key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        err = "failed to load tls private key: " + cfg.key_file;
        SSL_CTX_free(ctx);
        return false;
    }

    if (SSL_CTX_check_private_key(ctx) != 1) {
        err = "tls private key does not match certificate";
        SSL_CTX_free(ctx);
        return false;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        err = "socket() failed: " + std::string(std::strerror(errno));
        SSL_CTX_free(ctx);
        return false;
    }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        err = "setsockopt(SO_REUSEADDR) failed: " + std::string(std::strerror(errno));
        close(listen_fd);
        SSL_CTX_free(ctx);
        return false;
    }

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(cfg.port));
    addr.sin_addr = bind_addr;

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        err = "bind() failed: " + std::string(std::strerror(errno));
        close(listen_fd);
        SSL_CTX_free(ctx);
        return false;
    }

    if (listen(listen_fd, 16) != 0) {
        err = "listen() failed: " + std::string(std::strerror(errno));
        close(listen_fd);
        SSL_CTX_free(ctx);
        return false;
    }

    server.ctx = ctx;
    server.listen_fd = listen_fd;
    server.handshake_timeout_ms = cfg.handshake_timeout_ms;
    server.tag = cfg.tag;
    return true;
}

void close_tls_server(TlsServer& server) {
    if (server.listen_fd >= 0) {
        close(server.listen_fd);
        server.listen_fd = -1;
    }

    if (server.ctx != nullptr) {
        SSL_CTX_free(server.ctx);
        server.ctx = nullptr;
    }

    server.handshake_timeout_ms = 3000;
    server.tag.clear();
}

int accept_tls_client(const TlsServer& server,
                      TlsClientConnection& out_client,
                      sockaddr_in& out_peer,
                      std::string& err) {
    reset_client(out_client);
    out_peer = sockaddr_in {};

    if (server.ctx == nullptr || server.listen_fd < 0) {
        err = "tls server is not initialized";
        errno = EINVAL;
        return -1;
    }

    socklen_t peer_len = sizeof(out_peer);
    const int client_fd =
        accept(server.listen_fd, reinterpret_cast<sockaddr*>(&out_peer), &peer_len);
    if (client_fd < 0) {
        err = "accept() failed: " + std::string(std::strerror(errno));
        return -1;
    }

    SSL* ssl = SSL_new(server.ctx);
    if (ssl == nullptr) {
        err = "SSL_new failed";
        close(client_fd);
        errno = EIO;
        return -1;
    }

    if (SSL_set_fd(ssl, client_fd) != 1) {
        err = "SSL_set_fd failed";
        SSL_free(ssl);
        close(client_fd);
        errno = EIO;
        return -1;
    }

    if (!set_fd_nonblocking(client_fd, true)) {
        err = "failed to set nonblocking for handshake: " + std::string(std::strerror(errno));
        SSL_free(ssl);
        close(client_fd);
        return -1;
    }

    const auto start = std::chrono::steady_clock::now();
    const int timeout_ms = (server.handshake_timeout_ms > 0) ? server.handshake_timeout_ms : 3000;

    while (true) {
        const int rc = SSL_accept(ssl);
        if (rc == 1) {
            break;
        }

        const int ssl_error = SSL_get_error(ssl, rc);
        if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE) {
            err = "SSL_accept failed";
            SSL_free(ssl);
            close(client_fd);
            errno = EIO;
            return -1;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
        int remain = timeout_ms - static_cast<int>(elapsed.count());
        if (remain <= 0) {
            err = "TLS handshake timeout";
            SSL_free(ssl);
            close(client_fd);
            errno = ETIMEDOUT;
            return -1;
        }

        const int wait_ret = wait_ssl_io(client_fd, ssl_error, remain);
        if (wait_ret <= 0) {
            if (wait_ret == 0) {
                err = "TLS handshake timeout";
                errno = ETIMEDOUT;
            } else {
                err = "poll() during TLS handshake failed: " + std::string(std::strerror(errno));
            }
            SSL_free(ssl);
            close(client_fd);
            return -1;
        }
    }

    if (!set_fd_nonblocking(client_fd, false)) {
        err = "failed to restore blocking mode: " + std::string(std::strerror(errno));
        SSL_free(ssl);
        close(client_fd);
        return -1;
    }

    out_client.fd = client_fd;
    out_client.ssl = ssl;
    return client_fd;
}

ssize_t tls_read(const TlsClientConnection& client, void* buf, std::size_t len) {
    if (client.ssl == nullptr || client.fd < 0) {
        errno = EBADF;
        return -1;
    }

    const int rc = SSL_read(client.ssl, buf, static_cast<int>(len));
    if (rc > 0) return static_cast<ssize_t>(rc);

    const int ssl_error = SSL_get_error(client.ssl, rc);
    if (ssl_error == SSL_ERROR_ZERO_RETURN) {
        return 0;
    }
    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    if (ssl_error == SSL_ERROR_SYSCALL) {
        if (errno == 0) errno = EIO;
        return -1;
    }

    errno = EIO;
    return -1;
}

ssize_t tls_write(const TlsClientConnection& client, const void* data, std::size_t len) {
    if (client.ssl == nullptr || client.fd < 0) {
        errno = EBADF;
        return -1;
    }

    const int rc = SSL_write(client.ssl, data, static_cast<int>(len));
    if (rc > 0) return static_cast<ssize_t>(rc);

    const int ssl_error = SSL_get_error(client.ssl, rc);
    if (ssl_error == SSL_ERROR_ZERO_RETURN) {
        return 0;
    }
    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    if (ssl_error == SSL_ERROR_SYSCALL) {
        if (errno == 0) errno = EIO;
        return -1;
    }

    errno = EIO;
    return -1;
}

void close_tls_client(TlsClientConnection& client) {
    if (client.ssl != nullptr) {
        SSL_shutdown(client.ssl);
        SSL_free(client.ssl);
        client.ssl = nullptr;
    }

    if (client.fd >= 0) {
        close(client.fd);
        client.fd = -1;
    }
}
