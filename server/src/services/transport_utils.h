#ifndef APP_SERVICES_TRANSPORT_UTILS_H
#define APP_SERVICES_TRANSPORT_UTILS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include <poll.h>
#include <sys/types.h>

#include "app_services.h"
#include "tls_server.h"

namespace app_services_transport {

enum class TransportKind {
    Plain,
    Tls,
};

struct ListenerBundle {
    int plain_server_fd = -1;
    TlsServer tls_server;
};

struct AcceptedClient {
    TransportKind kind = TransportKind::Plain;
    int fd = -1;
    uint16_t port = 0;
    std::string ip;
    TlsClientConnection tls_conn {};
};

bool start_listener_bundle(ListenerBundle& bundle,
                           const SecurityRuntimeOptions& sec_cfg,
                           int plain_port,
                           int tls_port,
                           const char* service_tag,
                           const char* tls_tag,
                           bool plain_required,
                           bool tls_required);

void close_listener_bundle(ListenerBundle& bundle);
void append_listener_pollfds(const ListenerBundle& bundle, std::vector<pollfd>& pfds);
bool resolve_listener_kind(const ListenerBundle& bundle, int fd, TransportKind& kind);

bool accept_client(const ListenerBundle& bundle,
                   TransportKind kind,
                   const std::unordered_set<std::string>& allowlist,
                   const char* service_tag,
                   AcceptedClient& out_client);

void apply_read_timeout(const AcceptedClient& client, int timeout_ms);
ssize_t client_read(const AcceptedClient& client, void* buf, std::size_t len);
bool client_send_all(const AcceptedClient& client, const char* data, std::size_t len);
bool client_send_line(const AcceptedClient& client, const std::string& line);
void close_client(AcceptedClient& client);

const char* transport_name(TransportKind kind);

}  // namespace app_services_transport

#endif
