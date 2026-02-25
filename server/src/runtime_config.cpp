#include "runtime_config.h"

#include <cstdlib>
#include <string>

namespace {
bool load_required_env(const char* name, std::string& out, std::string& err) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        err = std::string("missing required env: ") + name;
        return false;
    }
    out = value;
    return true;
}

bool load_local_db_host(std::string& out, std::string& err) {
    const char* value = std::getenv("SFEPS_DB_HOST");
    if (value == nullptr || value[0] == '\0') {
        out = "localhost";
        return true;
    }

    out = value;
    if (out == "localhost") {
        return true;
    }

    err = "SFEPS_DB_HOST must be localhost (local-only mode)";
    return false;
}
} // namespace

bool load_runtime_config(RuntimeConfig& out, std::string& err) {
    if (!load_local_db_host(out.db_host, err)) return false;
    if (!load_required_env("SFEPS_DB_USER", out.db_user, err)) return false;
    if (!load_required_env("SFEPS_DB_PASS", out.db_pass, err)) return false;
    if (!load_required_env("SFEPS_DB_NAME_AUTH", out.db_name_auth, err)) return false;
    if (!load_required_env("SFEPS_DB_NAME_ANALYTICS", out.db_name_analytics, err)) return false;
    return true;
}
