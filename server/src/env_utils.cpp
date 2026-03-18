#include "env_utils.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <limits>

#include "text_utils.h"

std::size_t load_env_size_t(const char* name,
                            std::size_t default_value,
                            std::size_t min_value,
                            const char* log_prefix) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') return default_value;

    errno = 0;
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (errno != 0 || end == raw || (end != nullptr && *end != '\0') || parsed < min_value ||
        parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        std::cerr << (log_prefix ? log_prefix : "[env]") << " Invalid env " << name << "="
                  << raw << ", using default=" << default_value << std::endl;
        return default_value;
    }

    return static_cast<std::size_t>(parsed);
}

int load_env_int(const char* name, int default_value, int min_value, const char* log_prefix) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') return default_value;

    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(raw, &end, 10);
    if (errno != 0 || end == raw || (end != nullptr && *end != '\0') || parsed < min_value ||
        parsed > std::numeric_limits<int>::max()) {
        std::cerr << (log_prefix ? log_prefix : "[env]") << " Invalid env " << name << "="
                  << raw << ", using default=" << default_value << std::endl;
        return default_value;
    }

    return static_cast<int>(parsed);
}

bool load_env_bool(const char* name, bool default_value, const char* log_prefix) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') return default_value;

    std::string token = to_lower_copy(raw);
    if (token == "1" || token == "true" || token == "yes" || token == "on") return true;
    if (token == "0" || token == "false" || token == "no" || token == "off") return false;

    std::cerr << (log_prefix ? log_prefix : "[env]") << " Invalid boolean env " << name << "="
              << raw << ", using default=" << (default_value ? "1" : "0") << std::endl;
    return default_value;
}

int load_env_port(const char* name, int default_value, const char* log_prefix) {
    const int parsed = load_env_int(name, default_value, 1, log_prefix);
    if (parsed > 65535) {
        std::cerr << (log_prefix ? log_prefix : "[env]") << " Invalid port env " << name << "="
                  << parsed << ", using default=" << default_value << std::endl;
        return default_value;
    }
    return parsed;
}

std::string load_env_string(const char* name, const char* default_value) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') {
        return std::string(default_value != nullptr ? default_value : "");
    }
    return std::string(raw);
}

std::unordered_set<std::string> parse_allowlist_env(const char* name) {
    std::unordered_set<std::string> out;
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') return out;

    std::string csv(raw);
    std::size_t pos = 0;
    while (pos <= csv.size()) {
        const std::size_t comma = csv.find(',', pos);
        std::string token =
            (comma == std::string::npos) ? csv.substr(pos) : csv.substr(pos, comma - pos);
        token = trim_copy(token);
        if (!token.empty()) {
            out.insert(token);
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }

    return out;
}
