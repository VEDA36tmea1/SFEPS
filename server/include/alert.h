#ifndef ALERT_H
#define ALERT_H

#include <cstddef>
#include <string>

struct TlsClientConnection;

bool add_alert_plain_client(int fd);
bool add_alert_tls_client(TlsClientConnection&& client);
std::size_t alert_client_count();
void close_alert_client_connections();

void send_alert_to_clients(const std::string& msg);
void send_test_alert_to_clients(const std::string& msg = "TEST|PING");

#endif
