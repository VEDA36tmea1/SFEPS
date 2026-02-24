#ifndef DB_TLS_H
#define DB_TLS_H

#include <string>
#include <mysql/mysql.h>

// Configure MariaDB TLS options before mysql_real_connect().
// Returns false when DB_SSL_CA is missing or any TLS option fails.
bool configure_db_tls(MYSQL* conn, const char* component, std::string& err);

#endif
