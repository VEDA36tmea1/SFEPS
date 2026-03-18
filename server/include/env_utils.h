#ifndef ENV_UTILS_H
#define ENV_UTILS_H

#include <cstddef>
#include <string>
#include <unordered_set>

std::size_t load_env_size_t(const char* name,
                            std::size_t default_value,
                            std::size_t min_value,
                            const char* log_prefix);
int load_env_int(const char* name, int default_value, int min_value, const char* log_prefix);
bool load_env_bool(const char* name, bool default_value, const char* log_prefix);
int load_env_port(const char* name, int default_value, const char* log_prefix);
std::string load_env_string(const char* name, const char* default_value = "");
std::unordered_set<std::string> parse_allowlist_env(const char* name);

#endif
