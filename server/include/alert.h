#ifndef ALERT_H
#define ALERT_H

#include <string>

// send a text message to connected Qt clients (uses g_client_sockets from main)
void send_alert_to_clients(const std::string& msg);

#endif
