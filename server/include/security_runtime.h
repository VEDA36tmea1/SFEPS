#ifndef SECURITY_RUNTIME_H
#define SECURITY_RUNTIME_H

#include <string>
#include <unordered_set>

#include "app_services.h"

SecurityRuntimeOptions load_security_runtime_options();
bool validate_security_runtime_options(const SecurityRuntimeOptions& cfg, std::string& err);
void log_allowlist_mode(const char* env_name, const std::unordered_set<std::string>& allowlist);
void log_transport_mode(const SecurityRuntimeOptions& cfg);
void log_esp_transport_mode(const SecurityRuntimeOptions& cfg);

#endif
