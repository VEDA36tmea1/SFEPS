#ifndef NET_UTILS_H
#define NET_UTILS_H

#include <cstddef>
#include <string>
#include <unordered_set>

#include <netinet/in.h>

#include "tls_server.h"

bool is_ip_allowed(const std::unordered_set<std::string>& allowlist, const std::string& client_ip);
std::string peer_ip_to_string(const sockaddr_in& peer_addr);
void apply_socket_read_timeout(int fd, int timeout_ms);
int create_listen_socket(int port, const char* tag, const std::string& bind_ip);

bool send_all_plain(int fd, const char* data, std::size_t len);
bool send_all_tls(const TlsClientConnection& client, const char* data, std::size_t len);

#endif
