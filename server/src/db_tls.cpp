#include "db_tls.h"

#include <cstdlib>
#include <string>

namespace {
bool set_bool_option(MYSQL* conn, enum mysql_option option, my_bool value, const char* opt_name,
                     const char* component, std::string& err) {
    if (mysql_optionsv(conn, option, &value) == 0) return true;
    err = std::string(component) + ": failed to set " + opt_name + ": " + mysql_error(conn);
    return false;
}

bool set_str_option(MYSQL* conn, enum mysql_option option, const char* value, const char* opt_name,
                    const char* component, std::string& err) {
    if (mysql_optionsv(conn, option, value) == 0) return true;
    err = std::string(component) + ": failed to set " + opt_name + ": " + mysql_error(conn);
    return false;
}
} // namespace

bool configure_db_tls(MYSQL* conn, const char* component, std::string& err) {
    if (conn == NULL) {
        err = std::string(component) + ": null DB connection handle";
        return false;
    }

    const char* ca_path = std::getenv("DB_SSL_CA");
    if (ca_path == NULL || ca_path[0] == '\0') {
        err = std::string(component) + ": DB_SSL_CA is not set";
        return false;
    }

    const my_bool enforce_tls = 1;
    if (!set_bool_option(conn, MYSQL_OPT_SSL_ENFORCE, enforce_tls, "MYSQL_OPT_SSL_ENFORCE", component, err)) {
        return false;
    }

    const my_bool verify_server_cert = 1;
    if (!set_bool_option(conn, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, verify_server_cert,
                         "MYSQL_OPT_SSL_VERIFY_SERVER_CERT", component, err)) {
        return false;
    }

    if (!set_str_option(conn, MYSQL_OPT_SSL_CA, ca_path, "MYSQL_OPT_SSL_CA", component, err)) {
        return false;
    }

    static const char* kTlsVersions = "TLSv1.2,TLSv1.3";
    if (!set_str_option(conn, MYSQL_OPT_TLS_VERSION, kTlsVersions, "MYSQL_OPT_TLS_VERSION", component, err)) {
        return false;
    }

    return true;
}
