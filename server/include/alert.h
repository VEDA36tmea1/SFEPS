#ifndef ALERT_H
#define ALERT_H

#include <cstddef>
#include <string>

struct TlsClientConnection;

bool add_alert_plain_client(int fd, const std::string& client_ip);
bool add_alert_tls_client(TlsClientConnection&& client, const std::string& client_ip);
std::size_t alert_client_count();
void close_alert_client_connections();

void send_alert_to_clients(const std::string& msg);
void send_alert_to_ip_clients(const std::string& ip, const std::string& msg);

#endif
