#ifndef TLS_SERVER_H
#define TLS_SERVER_H

#include <cstddef>
#include <string>

#include <netinet/in.h>
#include <sys/types.h>
#include <openssl/ssl.h>

struct TlsServerConfig {
    int port = -1;
    std::string cert_file;
    std::string key_file;
    int handshake_timeout_ms = 3000;
    std::string tag;
};

struct TlsServer {
    SSL_CTX* ctx = nullptr;
    int listen_fd = -1;
    int handshake_timeout_ms = 3000;
    std::string tag;
};

struct TlsClientConnection {
    int fd = -1;
    SSL* ssl = nullptr;
};

bool init_tls_server(TlsServer& server, const TlsServerConfig& cfg, std::string& err);
void close_tls_server(TlsServer& server);

int accept_tls_client(const TlsServer& server,
                      TlsClientConnection& out_client,
                      sockaddr_in& out_peer,
                      std::string& err);

ssize_t tls_read(const TlsClientConnection& client, void* buf, std::size_t len);
ssize_t tls_write(const TlsClientConnection& client, const void* data, std::size_t len);
void close_tls_client(TlsClientConnection& client);

#endif
