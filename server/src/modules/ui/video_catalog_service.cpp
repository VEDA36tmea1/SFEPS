#include "app_services_impl.h"

#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

#include <mysql/mysql.h>

#include "recorder.h"
#include "service_shared.h"
#include "transport_utils.h"
#include "video_catalog_events.h"

namespace app_services_impl {

namespace {

using app_services_transport::AcceptedClient;
using app_services_transport::ListenerBundle;
using app_services_transport::TransportKind;

struct ClientState {
    AcceptedClient conn;
    std::string recv_buffer;
    std::uint64_t last_seen_seq = 0;
};

struct PollTarget {
    enum class Kind {
        Listener,
        Client,
    };

    Kind kind = Kind::Listener;
    std::size_t index = 0;
    TransportKind listener_kind = TransportKind::Plain;
};

struct VideoStorageStats {
    std::uintmax_t used_bytes = 0;
    std::uintmax_t total_bytes = 0;
    std::uintmax_t available_bytes = 0;
    std::size_t file_count = 0;
};

bool load_initial_catalog_records(const RuntimeConfig& cfg,
                                  std::vector<VideoCatalogRecordInfo>& out_records) {
    namespace fs = std::filesystem;

    MYSQL* conn = mysql_init(nullptr);
    if (conn == nullptr) {
        std::cerr << "[main.cpp] [VideoCatalog] mysql_init 실패." << std::endl;
        return false;
    }

    const bool connected =
        mysql_real_connect(conn,
                           cfg.db_host.c_str(),
                           cfg.db_user.c_str(),
                           cfg.db_pass.c_str(),
                           cfg.db_name_analytics.c_str(),
                           3306,
                           nullptr,
                           0) != nullptr;
    if (!connected) {
        std::cerr << "[main.cpp] [VideoCatalog] DB 연결 실패: " << mysql_error(conn)
                  << std::endl;
        mysql_close(conn);
        return false;
    }

    const char* query =
        "SELECT id, filename, DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') "
        "FROM recordings ORDER BY created_at DESC";
    if (mysql_query(conn, query) != 0) {
        std::cerr << "[main.cpp] [VideoCatalog] 초기 스냅샷 조회 실패: " << mysql_error(conn)
                  << std::endl;
        mysql_close(conn);
        return false;
    }

    MYSQL_RES* res = mysql_store_result(conn);
    if (res == nullptr) {
        std::cerr << "[main.cpp] [VideoCatalog] 초기 스냅샷 결과 조회 실패." << std::endl;
        mysql_close(conn);
        return false;
    }

    out_records.clear();
    while (MYSQL_ROW row = mysql_fetch_row(res)) {
        if (row[0] == nullptr || row[1] == nullptr || row[2] == nullptr) continue;

        VideoCatalogRecordInfo record;
        record.id = std::strtoll(row[0], nullptr, 10);
        record.filename = row[1];
        record.created_at = app_services_shared::normalize_to_iso8601(row[2]);
        if (record.id <= 0 || record.filename.empty() || record.created_at.empty()) continue;

        std::error_code ec;
        if (!fs::exists(record.filename, ec) || !fs::is_regular_file(record.filename, ec)) {
            continue;
        }
        out_records.push_back(std::move(record));
    }

    mysql_free_result(res);
    mysql_close(conn);
    return true;
}

std::string format_snapshot_begin_line(std::size_t total) {
    return "REC_SNAPSHOT_BEGIN|TOTAL=" + std::to_string(total) + "\n";
}

std::string format_snapshot_end_line(std::size_t total) {
    return "REC_SNAPSHOT_END|TOTAL=" + std::to_string(total) + "\n";
}

std::string format_rec_line(const VideoCatalogRecordInfo& record) {
    return "REC|" + std::to_string(record.id) + "|" + record.created_at + "\n";
}

std::string format_rec_add_line(const VideoCatalogRecordInfo& record) {
    return "REC_ADD|" + std::to_string(record.id) + "|" + record.created_at + "\n";
}

std::string format_rec_del_line(const VideoCatalogRecordInfo& record) {
    return "REC_DEL|" + std::to_string(record.id) + "\n";
}

std::string format_play_url_line(const VideoCatalogRecordInfo& record,
                                 const SecurityRuntimeOptions& sec_cfg) {
    namespace fs = std::filesystem;
    const std::string url = app_services_shared::join_http_url(
        sec_cfg.video_http_base_url, fs::path(record.filename).filename().string());
    return "PLAY_URL|" + std::to_string(record.id) + "|" + record.created_at + "|" + url + "\n";
}

std::string format_error_line(const char* prefix,
                              const std::string& code,
                              const std::string& message) {
    return std::string(prefix ? prefix : "REC_ERR") + "|" +
           app_services_shared::sanitize_error_field(code) + "|" +
           app_services_shared::sanitize_error_field(message) + "\n";
}

VideoStorageStats collect_storage_stats_from_records(
    const std::vector<VideoCatalogRecordInfo>& records) {
    namespace fs = std::filesystem;

    VideoStorageStats stats;
    for (const auto& record : records) {
        if (record.filename.empty()) continue;

        std::error_code ec;
        if (!fs::exists(record.filename, ec) || !fs::is_regular_file(record.filename, ec)) {
            continue;
        }

        const auto file_size = fs::file_size(record.filename, ec);
        if (ec) continue;

        stats.used_bytes += file_size;
        ++stats.file_count;
    }

    std::error_code ec;
    const fs::space_info space_info = fs::space(VIDEO_SAVE_DIR, ec);
    if (!ec) {
        stats.total_bytes = space_info.capacity;
        stats.available_bytes = space_info.available;
    }
    return stats;
}

VideoStorageStats collect_storage_stats_from_registry() {
    std::vector<VideoCatalogRecordInfo> records;
    (void)snapshot_video_catalog_registry(records);
    return collect_storage_stats_from_records(records);
}

std::string format_storage_line(const VideoStorageStats& stats) {
    return "REC_STORAGE|USED_BYTES=" +
           std::to_string(static_cast<unsigned long long>(stats.used_bytes)) +
           "|TOTAL_BYTES=" + std::to_string(static_cast<unsigned long long>(stats.total_bytes)) +
           "|AVAILABLE_BYTES=" +
           std::to_string(static_cast<unsigned long long>(stats.available_bytes)) +
           "|FILE_COUNT=" + std::to_string(stats.file_count) + "\n";
}

bool send_snapshot_to_client(ClientState& client) {
    std::vector<VideoCatalogRecordInfo> records;
    const std::uint64_t snapshot_seq = snapshot_video_catalog_registry(records);
    const VideoStorageStats storage_stats = collect_storage_stats_from_records(records);

    std::cout << "[main.cpp] [VideoCatalog] snapshot 전송 시작: ip=" << client.conn.ip
              << ", fd=" << client.conn.fd << ", total=" << records.size()
              << ", used_bytes=" << storage_stats.used_bytes
              << ", file_count=" << storage_stats.file_count << std::endl;

    if (!app_services_transport::client_send_line(
            client.conn, format_snapshot_begin_line(records.size()))) {
        std::cerr << "[main.cpp] [VideoCatalog] snapshot begin 전송 실패: ip=" << client.conn.ip
                  << ", fd=" << client.conn.fd << std::endl;
        return false;
    }
    for (const auto& record : records) {
        if (!app_services_transport::client_send_line(client.conn, format_rec_line(record))) {
            std::cerr << "[main.cpp] [VideoCatalog] snapshot record 전송 실패: ip="
                      << client.conn.ip << ", fd=" << client.conn.fd
                      << ", id=" << record.id << std::endl;
            return false;
        }
    }
    if (!app_services_transport::client_send_line(
            client.conn, format_snapshot_end_line(records.size()))) {
        std::cerr << "[main.cpp] [VideoCatalog] snapshot end 전송 실패: ip=" << client.conn.ip
                  << ", fd=" << client.conn.fd << std::endl;
        return false;
    }
    if (!app_services_transport::client_send_line(client.conn, format_storage_line(storage_stats))) {
        std::cerr << "[main.cpp] [VideoCatalog] snapshot storage 전송 실패: ip="
                  << client.conn.ip << ", fd=" << client.conn.fd << std::endl;
        return false;
    }

    client.last_seen_seq = snapshot_seq;
    std::cout << "[main.cpp] [VideoCatalog] snapshot 전송 완료: ip=" << client.conn.ip
              << ", fd=" << client.conn.fd << ", total=" << records.size()
              << ", snapshot_seq=" << client.last_seen_seq
              << ", used_bytes=" << storage_stats.used_bytes
              << ", total_bytes=" << storage_stats.total_bytes
              << ", available_bytes=" << storage_stats.available_bytes
              << ", file_count=" << storage_stats.file_count << std::endl;
    return true;
}

bool parse_play_request_id(const std::string& line, long long& out_id, std::string& out_error) {
    const std::string trimmed = app_services_shared::trim_copy(line);
    if (trimmed.empty()) {
        out_error = "empty request";
        return false;
    }

    const std::string prefix = "PLAY_REC|";
    if (trimmed.rfind(prefix, 0) != 0) {
        out_error = "expected PLAY_REC|<id>";
        return false;
    }

    const std::string raw_id = app_services_shared::trim_copy(trimmed.substr(prefix.size()));
    if (raw_id.empty()) {
        out_error = "missing id";
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(raw_id.c_str(), &end, 10);
    if (errno != 0 || end == raw_id.c_str() || (end != nullptr && *end != '\0') || parsed <= 0) {
        out_error = "id must be positive integer";
        return false;
    }

    out_id = parsed;
    return true;
}

bool handle_play_request(ClientState& client,
                         const SecurityRuntimeOptions& sec_cfg,
                         long long record_id) {
    namespace fs = std::filesystem;

    std::cout << "[main.cpp] [VideoCatalog] PLAY_REC 수신: ip=" << client.conn.ip
              << ", fd=" << client.conn.fd << ", id=" << record_id << std::endl;

    VideoCatalogRecordInfo record;
    if (!find_video_catalog_record_by_id(record_id, record)) {
        std::cerr << "[main.cpp] [VideoCatalog] PLAY_REC NOT_FOUND: ip=" << client.conn.ip
                  << ", fd=" << client.conn.fd << ", id=" << record_id
                  << ", reason=id_not_found" << std::endl;
        return app_services_transport::client_send_line(
            client.conn, format_error_line("PLAY_ERR", "NOT_FOUND", "recording id not found"));
    }

    std::error_code ec;
    if (!fs::exists(record.filename, ec) || !fs::is_regular_file(record.filename, ec)) {
        publish_video_catalog_record_deleted_by_id(record.id);
        std::cerr << "[main.cpp] [VideoCatalog] PLAY_REC NOT_FOUND: ip=" << client.conn.ip
                  << ", fd=" << client.conn.fd << ", id=" << record.id
                  << ", reason=file_missing, filename=" << record.filename << std::endl;
        return app_services_transport::client_send_line(
            client.conn, format_error_line("PLAY_ERR", "NOT_FOUND", "recording file missing"));
    }

    std::cout << "[main.cpp] [VideoCatalog] PLAY_URL 응답 성공: ip=" << client.conn.ip
              << ", fd=" << client.conn.fd << ", id=" << record.id
              << ", created_at=" << record.created_at << ", filename=" << record.filename
              << std::endl;
    return app_services_transport::client_send_line(
        client.conn, format_play_url_line(record, sec_cfg));
}

bool handle_client_input(ClientState& client,
                         std::atomic<bool>& running,
                         const SecurityRuntimeOptions& sec_cfg) {
    char chunk[1024];
    const ssize_t n = app_services_transport::client_read(client.conn, chunk, sizeof(chunk));
    if (n == 0) {
        return false;
    }
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        return false;
    }

    client.recv_buffer.append(chunk, static_cast<std::size_t>(n));
    if (client.recv_buffer.size() > app_services_shared::kMaxVideoCatalogRequestBytes) {
        app_services_transport::client_send_line(
            client.conn,
            format_error_line("PLAY_ERR", "PAYLOAD_TOO_LARGE", "request exceeds maximum size"));
        return false;
    }

    while (running.load()) {
        const std::size_t newline = client.recv_buffer.find('\n');
        if (newline == std::string::npos) break;

        const std::string line =
            app_services_shared::trim_copy(client.recv_buffer.substr(0, newline));
        client.recv_buffer.erase(0, newline + 1);
        if (line.empty()) continue;

        long long record_id = 0;
        std::string error;
        if (!parse_play_request_id(line, record_id, error)) {
            std::cerr << "[main.cpp] [VideoCatalog] PLAY_REC 잘못된 요청: ip="
                      << client.conn.ip << ", fd=" << client.conn.fd
                      << ", raw='" << line << "', error=" << error << std::endl;
            if (!app_services_transport::client_send_line(
                    client.conn, format_error_line("PLAY_ERR", "INVALID_REQUEST", error))) {
                return false;
            }
            continue;
        }

        if (!handle_play_request(client, sec_cfg, record_id)) {
            return false;
        }
    }

    return true;
}

bool sync_client_events(ClientState& client) {
    std::vector<VideoCatalogEvent> events;
    collect_video_catalog_events_since(client.last_seen_seq, events);
    bool sent_any_event = false;

    for (const auto& event : events) {
        std::string line;
        if (event.kind == VideoCatalogEvent::Kind::Added) {
            line = format_rec_add_line(event.record);
        } else {
            line = format_rec_del_line(event.record);
        }

        if (!app_services_transport::client_send_line(client.conn, line)) {
            std::cerr << "[main.cpp] [VideoCatalog] 실시간 이벤트 전송 실패: ip="
                      << client.conn.ip << ", fd=" << client.conn.fd
                      << ", seq=" << event.seq << ", kind="
                      << (event.kind == VideoCatalogEvent::Kind::Added ? "ADD" : "DEL")
                      << ", id=" << event.record.id << std::endl;
            return false;
        }
        std::cout << "[main.cpp] [VideoCatalog] 실시간 이벤트 전송: ip=" << client.conn.ip
                  << ", fd=" << client.conn.fd << ", seq=" << event.seq << ", kind="
                  << (event.kind == VideoCatalogEvent::Kind::Added ? "ADD" : "DEL")
                  << ", id=" << event.record.id << std::endl;
        client.last_seen_seq = event.seq;
        sent_any_event = true;
    }

    if (sent_any_event) {
        const VideoStorageStats storage_stats = collect_storage_stats_from_registry();
        if (!app_services_transport::client_send_line(client.conn,
                                                      format_storage_line(storage_stats))) {
            std::cerr << "[main.cpp] [VideoCatalog] REC_STORAGE 전송 실패: ip="
                      << client.conn.ip << ", fd=" << client.conn.fd << std::endl;
            return false;
        }
        std::cout << "[main.cpp] [VideoCatalog] REC_STORAGE 전송: ip=" << client.conn.ip
                  << ", fd=" << client.conn.fd
                  << ", used_bytes=" << storage_stats.used_bytes
                  << ", total_bytes=" << storage_stats.total_bytes
                  << ", available_bytes=" << storage_stats.available_bytes
                  << ", file_count=" << storage_stats.file_count << std::endl;
    }

    return true;
}

}  // namespace

void run_video_catalog_service_impl(std::atomic<bool>& running,
                                    const RuntimeConfig& cfg,
                                    const SecurityRuntimeOptions& sec_cfg) {
    using namespace app_services_transport;
    const auto& shared_alert_video_allow_ips = sec_cfg.alert_allow_ips;

    std::vector<VideoCatalogRecordInfo> initial_records;
    if (!load_initial_catalog_records(cfg, initial_records)) {
        std::cerr << "[main.cpp] [VideoCatalog] 초기 카탈로그 로드 실패. 서비스 비활성화."
                  << std::endl;
        return;
    }
    seed_video_catalog_registry(initial_records);

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

    std::vector<ClientState> clients;
    while (running.load()) {
        std::vector<pollfd> pfds;
        std::vector<PollTarget> targets;
        append_listener_pollfds(listeners, pfds);
        if (listeners.plain_server_fd >= 0) {
            targets.push_back(
                PollTarget {PollTarget::Kind::Listener, 0, TransportKind::Plain});
        }
        if (listeners.tls_server.listen_fd >= 0) {
            targets.push_back(
                PollTarget {PollTarget::Kind::Listener, 0, TransportKind::Tls});
        }
        for (std::size_t i = 0; i < clients.size(); ++i) {
            pfds.push_back(pollfd {clients[i].conn.fd, POLLIN, 0});
            targets.push_back(PollTarget {PollTarget::Kind::Client, i, TransportKind::Plain});
        }

        const int poll_ret = poll(pfds.data(), pfds.size(), 250);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[VideoCatalog] poll() 실패: " << std::strerror(errno) << std::endl;
            break;
        }

        std::vector<std::size_t> remove_indices;
        if (poll_ret > 0) {
            for (std::size_t i = 0; i < pfds.size() && running.load(); ++i) {
                if (pfds[i].revents == 0) continue;

                const PollTarget& target = targets[i];
                if (target.kind == PollTarget::Kind::Listener) {
                    AcceptedClient accepted;
                    if (!accept_client(listeners,
                                       target.listener_kind,
                                       shared_alert_video_allow_ips,
                                       "VideoCatalog",
                                       accepted)) {
                        continue;
                    }

                    if (clients.size() >= sec_cfg.video_max_clients) {
                        client_send_line(accepted,
                                         format_error_line("REC_ERR",
                                                           "MAX_CLIENTS",
                                                           "video catalog max clients reached"));
                        close_client(accepted);
                        continue;
                    }

                    apply_read_timeout(accepted, sec_cfg.socket_read_timeout_ms);
                    ClientState client;
                    client.conn = std::move(accepted);
                    if (!send_snapshot_to_client(client)) {
                        close_client(client.conn);
                        continue;
                    }

                    std::cout << "[main.cpp] [VideoCatalog] 구독 연결 완료: ip="
                              << client.conn.ip << ", fd=" << client.conn.fd
                              << ", snapshot_seq=" << client.last_seen_seq << std::endl;
                    clients.push_back(std::move(client));
                    continue;
                }

                if ((pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    remove_indices.push_back(target.index);
                    continue;
                }
                if ((pfds[i].revents & POLLIN) == 0) continue;

                if (target.index >= clients.size()) continue;
                if (!handle_client_input(clients[target.index], running, sec_cfg)) {
                    remove_indices.push_back(target.index);
                }
            }
        }

        for (std::size_t i = 0; i < clients.size(); ++i) {
            if (!sync_client_events(clients[i])) {
                remove_indices.push_back(i);
            }
        }

        std::sort(remove_indices.begin(), remove_indices.end());
        remove_indices.erase(std::unique(remove_indices.begin(), remove_indices.end()),
                             remove_indices.end());
        std::reverse(remove_indices.begin(), remove_indices.end());

        for (std::size_t index : remove_indices) {
            if (index >= clients.size()) continue;
            close_client(clients[index].conn);
            clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    for (auto& client : clients) {
        close_client(client.conn);
    }
    close_listener_bundle(listeners);
    std::cout << "[main.cpp] [VideoCatalog] 종료." << std::endl;
}

}  // namespace app_services_impl
