#include "app_services_impl.h"

#include <poll.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

#include <mysql/mysql.h>

#include "service_shared.h"
#include "transport_utils.h"

namespace app_services_impl {

namespace {

bool read_request_line(const app_services_transport::AcceptedClient& client,
                       std::atomic<bool>& running,
                       std::string& out_request,
                       bool& out_oversized) {
    using namespace app_services_shared;
    using namespace app_services_transport;

    out_request.clear();
    out_oversized = false;

    char chunk[1024];
    while (running.load()) {
        const ssize_t n = client_read(client, chunk, sizeof(chunk));
        if (n > 0) {
            out_request.append(chunk, static_cast<std::size_t>(n));
            if (out_request.size() > kMaxVideoCatalogRequestBytes) {
                out_oversized = true;
                break;
            }
            if (out_request.find('\n') != std::string::npos) break;
            continue;
        }
        if (n == 0) break;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return false;
    }

    if (out_request.empty() && !out_oversized) return false;
    const std::size_t newline = out_request.find('\n');
    if (newline != std::string::npos) out_request.resize(newline);
    out_request = trim_copy(out_request);
    return true;
}

}  // namespace

void run_video_catalog_service_impl(std::atomic<bool>& running,
                                    const RuntimeConfig& cfg,
                                    const SecurityRuntimeOptions& sec_cfg) {
    using namespace app_services_shared;
    using namespace app_services_transport;

    namespace fs = std::filesystem;

    ListenerBundle listeners;
    if (!start_listener_bundle(listeners,
                               sec_cfg,
                               sec_cfg.video_catalog_port,
                               sec_cfg.video_catalog_tls_port,
                               "VideoCatalog",
                               "VideoCatalogTLS",
                               false,
                               false)) {
        std::cerr << "[main.cpp] [VideoCatalog] 사용 가능한 리스너 없음. 서비스 비활성화."
                  << std::endl;
        return;
    }

    MYSQL* db_conn = nullptr;
    auto close_db = [&]() {
        if (db_conn != nullptr) {
            mysql_close(db_conn);
            db_conn = nullptr;
        }
    };

    auto ensure_db = [&]() -> bool {
        if (db_conn != nullptr) return true;

        db_conn = mysql_init(nullptr);
        if (db_conn == nullptr) {
            std::cerr << "[main.cpp] [VideoCatalog] mysql_init 실패." << std::endl;
            return false;
        }

        if (mysql_real_connect(db_conn,
                               cfg.db_host.c_str(),
                               cfg.db_user.c_str(),
                               cfg.db_pass.c_str(),
                               cfg.db_name_analytics.c_str(),
                               3306,
                               nullptr,
                               0) == nullptr) {
            std::cerr << "[main.cpp] [VideoCatalog] DB 연결 실패: " << mysql_error(db_conn)
                      << std::endl;
            close_db();
            return false;
        }

        return true;
    };

    auto send_error = [&](const AcceptedClient& client,
                          const std::string& code,
                          const std::string& msg) {
        const std::string line =
            "REC_ERR|" + sanitize_error_field(code) + "|" + sanitize_error_field(msg) + "\n";
        (void)client_send_line(client, line);
    };

    auto handle_request = [&](const AcceptedClient& client, const std::string& request_line) {
        VideoCatalogRequest request;
        std::string parse_code;
        std::string parse_msg;
        if (!parse_video_catalog_request(request_line, request, parse_code, parse_msg)) {
            send_error(client, parse_code, parse_msg);
            return false;
        }

        if (!ensure_db()) {
            send_error(client, "DB_UNAVAILABLE", "database connection failed");
            return false;
        }

        std::vector<std::string> filters;
        filters.push_back("1=1");
        if (!request.from.empty()) {
            filters.push_back("created_at >= '" + mysql_escape_literal(db_conn, request.from) + "'");
        }
        if (!request.to.empty()) {
            filters.push_back("created_at <= '" + mysql_escape_literal(db_conn, request.to) + "'");
        }
        if (!request.q.empty()) {
            filters.push_back("filename LIKE '%" + mysql_escape_literal(db_conn, request.q) + "%'"
            );
        }

        std::string where_sql;
        for (std::size_t i = 0; i < filters.size(); ++i) {
            if (i > 0) where_sql += " AND ";
            where_sql += filters[i];
        }

        long long total_rows = 0;
        const std::string count_sql = "SELECT COUNT(*) FROM recordings WHERE " + where_sql;
        if (mysql_query(db_conn, count_sql.c_str()) != 0) {
            send_error(client, "DB_ERROR", mysql_error(db_conn));
            close_db();
            return false;
        }

        MYSQL_RES* count_res = mysql_store_result(db_conn);
        if (count_res == nullptr) {
            send_error(client, "DB_ERROR", "failed to fetch count result");
            close_db();
            return false;
        }

        MYSQL_ROW count_row = mysql_fetch_row(count_res);
        if (count_row != nullptr && count_row[0] != nullptr) {
            total_rows = std::strtoll(count_row[0], nullptr, 10);
            if (total_rows < 0) total_rows = 0;
        }
        mysql_free_result(count_res);

        const long long offset =
            static_cast<long long>(request.page - 1) * static_cast<long long>(request.size);
        const std::string list_sql =
            "SELECT id, filename, DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') "
            "FROM recordings WHERE " +
            where_sql + " ORDER BY created_at DESC LIMIT " + std::to_string(offset) + ", " +
            std::to_string(request.size);

        if (mysql_query(db_conn, list_sql.c_str()) != 0) {
            send_error(client, "DB_ERROR", mysql_error(db_conn));
            close_db();
            return false;
        }

        MYSQL_RES* list_res = mysql_store_result(db_conn);
        if (list_res == nullptr) {
            send_error(client, "DB_ERROR", "failed to fetch recordings result");
            close_db();
            return false;
        }

        std::size_t sent_records = 0;
        while (running.load()) {
            MYSQL_ROW row = mysql_fetch_row(list_res);
            if (row == nullptr) break;
            if (row[0] == nullptr || row[1] == nullptr || row[2] == nullptr) continue;

            const std::string id = row[0];
            const std::string filename = row[1];
            const std::string created_at = row[2];

            std::error_code ec;
            if (!fs::exists(filename, ec) || !fs::is_regular_file(filename, ec)) {
                continue;
            }

            const std::string play_url =
                join_http_url(sec_cfg.video_http_base_url, fs::path(filename).filename().string());
            const std::string rec_line =
                "REC|" + id + "|" + normalize_to_iso8601(created_at) + "|0|" + play_url + "\n";
            if (!client_send_line(client, rec_line)) {
                mysql_free_result(list_res);
                return false;
            }
            ++sent_records;
        }
        mysql_free_result(list_res);

        const int has_next = (offset + static_cast<long long>(request.size) < total_rows) ? 1 : 0;
        const std::string end_line = "REC_END|PAGE=" + std::to_string(request.page) +
                                     "|SIZE=" + std::to_string(request.size) +
                                     "|TOTAL=" + std::to_string(total_rows) +
                                     "|HAS_NEXT=" + std::to_string(has_next) + "\n";
        if (!client_send_line(client, end_line)) return false;

        std::cout << "[main.cpp] [VideoCatalog] 요청 처리: ip=" << client.ip
                  << ", page=" << request.page << ", size=" << request.size
                  << ", sent=" << sent_records << ", total=" << total_rows << std::endl;
        return true;
    };

    std::size_t active_requests = 0;
    while (running.load()) {
        std::vector<pollfd> pfds;
        append_listener_pollfds(listeners, pfds);
        if (pfds.empty()) break;

        const int poll_ret = poll(pfds.data(), pfds.size(), 1000);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[VideoCatalog] poll() 실패: " << std::strerror(errno) << std::endl;
            break;
        }
        if (poll_ret == 0) continue;

        for (const auto& pfd : pfds) {
            if ((pfd.revents & POLLIN) == 0) continue;

            TransportKind kind;
            if (!resolve_listener_kind(listeners, pfd.fd, kind)) continue;

            AcceptedClient client;
            if (!accept_client(listeners, kind, sec_cfg.alert_allow_ips, "VideoCatalog", client)) {
                if (!running.load()) break;
                continue;
            }

            if (active_requests >= sec_cfg.video_max_clients) {
                send_error(client, "MAX_CLIENTS", "video catalog max clients reached");
                close_client(client);
                continue;
            }

            apply_read_timeout(client, sec_cfg.socket_read_timeout_ms);

            std::string request_line;
            bool oversized = false;
            if (!read_request_line(client, running, request_line, oversized)) {
                close_client(client);
                continue;
            }
            if (oversized) {
                send_error(client, "PAYLOAD_TOO_LARGE", "request exceeds maximum size");
                close_client(client);
                continue;
            }

            ++active_requests;
            handle_request(client, request_line);
            if (active_requests > 0) --active_requests;

            close_client(client);
        }
    }

    close_listener_bundle(listeners);
    close_db();
    std::cout << "[main.cpp] [VideoCatalog] 서비스 스레드 종료." << std::endl;
}

}  // namespace app_services_impl
