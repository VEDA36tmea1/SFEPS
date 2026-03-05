#ifndef RUNTIME_CONFIG_H
#define RUNTIME_CONFIG_H

#include <string>

struct RuntimeConfig {
    std::string db_host;
    std::string db_user;
    std::string db_pass;
    std::string db_name_analytics;
};

bool load_runtime_config(RuntimeConfig& out, std::string& err);

#endif
