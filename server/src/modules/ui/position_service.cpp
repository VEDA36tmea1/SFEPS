#include "app_services_impl.h"

#include <poll.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "alert.h"
#include "esp_manager.h"
#include "service_shared.h"
#include "transport_utils.h"

namespace app_services_impl {

void run_position_stream_service_impl(std::atomic<bool>& running,
                                      const SecurityRuntimeOptions& sec_cfg,
                                      AnalyticsProcessor& analytics,
                                      EspManager& esp_manager) {
    using namespace app_services_shared;
    using namespace app_services_transport;

    struct ClientState {
        AcceptedClient conn;
        std::string recv_buffer;
        std::string active_object_id;
    };

    struct PollTarget {
        enum class Kind {
            Listener,
            Client,
        };

        Kind kind;
        std::size_t index;
        TransportKind listener_kind = TransportKind::Plain;
    };

    ListenerBundle listeners;
    if (!start_listener_bundle(listeners,
                               sec_cfg,
                               kPositionPort,
                               sec_cfg.position_tls_port,
                               "Position",
                               "PositionTLS",
                               true,
                               true)) {
        running = false;
        return;
    }

    std::vector<ClientState> clients;
    std::string esp_active_object_id;
    bool esp_has_last_sent = false;
    AnalyticsProcessor::ObjectPositionSnapshot esp_last_sent;
    std::chrono::steady_clock::time_point esp_last_sent_at =
        std::chrono::steady_clock::time_point::min();

    struct ObjLastSentState {
        std::chrono::steady_clock::time_point updated_at;
        std::chrono::steady_clock::time_point sent_at;
        bool is_fraud = false;
    };

    std::unordered_map<std::string, ObjLastSentState> obj_last_sent;
    std::vector<AnalyticsProcessor::ObjectPositionSnapshot> obj_snapshots;
    std::unordered_set<std::string> obj_ids_this_tick;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> pending_deauth;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        last_force_logout_sent_at;

    const auto deauth_grace =
        std::chrono::milliseconds(std::max(0, sec_cfg.auth_deauth_grace_ms));
    const auto force_logout_cooldown = std::chrono::seconds(5);
    const auto esp_min_send_interval = std::chrono::seconds(1);
    const auto esp_test_track_pos_interval =
        std::chrono::seconds(std::max(1, sec_cfg.esp_test_track_pos_interval_sec));
    auto esp_test_track_pos_next_at = std::chrono::steady_clock::now();

    constexpr std::size_t kMaxRecvBuffer = 16 * 1024;
    constexpr std::size_t kReadBufferSize = 4096;
    char read_buffer[kReadBufferSize];

    auto mark_remove = [](std::vector<std::size_t>& remove_indices, std::size_t index) {
        remove_indices.push_back(index);
    };

    auto unique_descending = [](std::vector<std::size_t>& indices) {
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        std::reverse(indices.begin(), indices.end());
    };

    const auto active_position_connections_for_ip = [&](const std::string& ip) -> std::size_t {
        if (ip.empty()) return 0;

        std::size_t count = 0;
        for (const auto& c : clients) {
            if (c.conn.ip == ip) ++count;
        }
        return count;
    };

    const auto schedule_deauth = [&](const std::string& ip, const char* reason) {
        if (ip.empty()) return;
        const auto due = std::chrono::steady_clock::now() + deauth_grace;
        pending_deauth[ip] = due;
        std::cout << "[main.cpp] [Auth] 인증 해제 예약: ip=" << ip
                  << ", grace_ms=" << sec_cfg.auth_deauth_grace_ms
                  << ", reason=" << (reason ? reason : "disconnect") << std::endl;
    };

    const auto cancel_pending_deauth = [&](const std::string& ip) {
        if (ip.empty()) return;
        if (pending_deauth.erase(ip) > 0) {
            std::cout << "[main.cpp] [Auth] 인증 해제 취소(재연결): ip=" << ip << std::endl;
        }
    };

    const auto run_pending_deauth = [&]() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = pending_deauth.begin(); it != pending_deauth.end();) {
            const std::string ip = it->first;
            if (it->second > now) {
                ++it;
                continue;
            }

            if (active_position_connections_for_ip(ip) > 0) {
                std::cout << "[main.cpp] [Auth] 인증 해제 건너뜀(활성 position 연결): ip="
                          << ip << std::endl;
                it = pending_deauth.erase(it);
                continue;
            }

            const bool removed = unmark_ip_authenticated(ip);
            if (removed) {
                std::cout << "[main.cpp] [Auth] 인증 세션 해제: ip=" << ip << std::endl;
            }
            it = pending_deauth.erase(it);
        }
    };

    const auto send_force_logout_event = [&](const std::string& ip, const char* proto) {
        if (ip.empty()) return;

        const auto now = std::chrono::steady_clock::now();
        const auto sent_it = last_force_logout_sent_at.find(ip);
        if (sent_it != last_force_logout_sent_at.end() &&
            (now - sent_it->second) < force_logout_cooldown) {
            std::cout << "[main.cpp] [Auth] 강제 로그아웃 이벤트 생략(쿨다운): ip=" << ip
                      << ", proto=" << (proto ? proto : "UNKNOWN") << std::endl;
            return;
        }

        const std::string logout_msg = "AUTH|FORCE_LOGOUT|REASON=POSITION_UNAUTHENTICATED|PROTO=" +
                                       std::string(proto ? proto : "UNKNOWN") + "\n";
        send_alert_to_ip_clients(ip, logout_msg);
        last_force_logout_sent_at[ip] = now;
        std::cout << "[main.cpp] [Auth] 강제 로그아웃 이벤트 전송: ip=" << ip
                  << ", proto=" << (proto ? proto : "UNKNOWN") << std::endl;
    };

    const auto try_publish_esp_track_pos = [&](const std::string& object_id) -> bool {
        if (object_id.empty()) return false;

        AnalyticsProcessor::ObjectPositionSnapshot snapshot;
        if (!analytics.getObjectPositionSnapshot(object_id, snapshot)) {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto esp_stale_limit =
            std::chrono::seconds(static_cast<long long>(sec_cfg.position_stale_seconds));
        if ((now - snapshot.updated_at) > esp_stale_limit) {
            return false;
        }
        if (esp_has_last_sent && (now - esp_last_sent_at) < esp_min_send_interval) {
            return false;
        }

        EspManager::TrackPosPayload payload;
        payload.object_id = snapshot.object_id;
        payload.left = snapshot.left;
        payload.top = snapshot.top;
        payload.right = snapshot.right;
        payload.bottom = snapshot.bottom;
        payload.x = snapshot.x;
        payload.y = snapshot.y;
        payload.tag_time = snapshot.tag_time;
        if (!esp_manager.publishTrackPos(payload)) {
            return false;
        }

        esp_last_sent = snapshot;
        esp_has_last_sent = true;
        esp_last_sent_at = now;
        return true;
    };

    const auto switch_esp_track_target = [&](const std::string& requested_id) {
        if (requested_id.empty()) return;
        if (esp_active_object_id == requested_id) return;

        const bool was_tracking = !esp_active_object_id.empty();
        if (was_tracking) {
            esp_manager.publishTrackChangeSignal(esp_active_object_id, requested_id);
            esp_manager.publishTrackEnd(esp_active_object_id, "SWITCH");
        }

        esp_active_object_id = requested_id;
        esp_has_last_sent = false;
        esp_last_sent_at = std::chrono::steady_clock::time_point::min();
        esp_manager.setClientTrackObjectId(esp_active_object_id);
        esp_manager.publishTrackStart(esp_active_object_id);
        try_publish_esp_track_pos(esp_active_object_id);
    };

    const auto clear_esp_track_target = [&](const std::string& requested_id, const char* reason) {
        if (requested_id.empty()) return;
        if (esp_active_object_id == requested_id) {
            const std::string reason_text = reason ? reason : "UNSUB";
            esp_manager.publishTrackEnd(esp_active_object_id, reason_text);
            esp_active_object_id.clear();
            esp_has_last_sent = false;
            esp_last_sent_at = std::chrono::steady_clock::time_point::min();
            esp_manager.clearClientTrackObjectId();
        }
    };

    const auto reconcile_esp_track_target = [&]() {
        if (esp_active_object_id.empty()) return;

        bool still_requested = false;
        for (const auto& c : clients) {
            if (c.active_object_id == esp_active_object_id) {
                still_requested = true;
                break;
            }
        }
        if (still_requested) return;

        std::string next_target;
        for (const auto& c : clients) {
            if (c.conn.kind == TransportKind::Plain && !c.active_object_id.empty()) {
                next_target = c.active_object_id;
                break;
            }
        }
        if (next_target.empty()) {
            for (const auto& c : clients) {
                if (c.conn.kind == TransportKind::Tls && !c.active_object_id.empty()) {
                    next_target = c.active_object_id;
                    break;
                }
            }
        }

        if (next_target.empty()) {
            std::cout << "[main.cpp] [Position] 활성 구독자 없음: ESP 추적 대상 해제"
                      << std::endl;
            clear_esp_track_target(esp_active_object_id, "NO_SUBSCRIBER");
            return;
        }

        std::cout << "[main.cpp] [Position] 연결 해제 후 ESP 추적 대상 전환: 이전="
                  << esp_active_object_id << ", to=" << next_target << std::endl;
        switch_esp_track_target(next_target);
    };

    while (running.load()) {
        std::vector<pollfd> pfds;
        std::vector<PollTarget> targets;

        if (listeners.plain_server_fd >= 0) {
            pfds.push_back(pollfd {listeners.plain_server_fd, POLLIN, 0});
            targets.push_back(PollTarget {PollTarget::Kind::Listener, 0, TransportKind::Plain});
        }
        if (listeners.tls_server.listen_fd >= 0) {
            pfds.push_back(pollfd {listeners.tls_server.listen_fd, POLLIN, 0});
            targets.push_back(PollTarget {PollTarget::Kind::Listener, 0, TransportKind::Tls});
        }
        for (std::size_t i = 0; i < clients.size(); ++i) {
            pfds.push_back(pollfd {clients[i].conn.fd, POLLIN | POLLHUP | POLLERR, 0});
            targets.push_back(PollTarget {PollTarget::Kind::Client, i, TransportKind::Plain});
        }

        const int tick_ms = std::max(1, sec_cfg.position_stream_tick_ms);
        const int poll_ret = poll(pfds.data(), pfds.size(), tick_ms);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Position] poll() 실패: " << std::strerror(errno) << std::endl;
            break;
        }

        std::vector<std::size_t> remove_clients;

        if (poll_ret > 0) {
            for (std::size_t i = 0; i < pfds.size(); ++i) {
                const short revents = pfds[i].revents;
                if (revents == 0) continue;

                const PollTarget target = targets[i];

                if (target.kind == PollTarget::Kind::Listener) {
                    if ((revents & POLLIN) == 0) continue;

                    AcceptedClient accepted;
                    if (!accept_client(
                            listeners, target.listener_kind, sec_cfg.alert_allow_ips, "Position", accepted)) {
                        if (!running.load()) break;
                        continue;
                    }

                    if (!is_ip_authenticated(accepted.ip)) {
                        std::cout << "[main.cpp] [Position] " << transport_name(accepted.kind)
                                  << " connection rejected: unauthenticated ip=" << accepted.ip
                                  << std::endl;
                        send_force_logout_event(
                            accepted.ip,
                            accepted.kind == TransportKind::Plain ? "PLAIN" : "TLS");
                        close_client(accepted);
                        continue;
                    }

                    apply_read_timeout(accepted, sec_cfg.socket_read_timeout_ms);

                    if (clients.size() >= sec_cfg.position_max_clients) {
                        std::cout << "[main.cpp] [Position] " << transport_name(accepted.kind)
                                  << " connection rejected: max clients reached ("
                                  << sec_cfg.position_max_clients << ")" << std::endl;
                        close_client(accepted);
                        continue;
                    }

                    cancel_pending_deauth(accepted.ip);
                    const char* label = accepted.kind == TransportKind::Plain ? "plain" : "TLS";
                    std::cout << "[main.cpp] [Position] " << label
                              << " client connected: " << accepted.ip << ":" << accepted.port
                              << " (fd=" << accepted.fd << ")" << std::endl;
                    clients.push_back(ClientState {std::move(accepted), "", ""});
                    continue;
                }

                if (target.index >= clients.size()) continue;
                auto& client = clients[target.index];

                if ((revents & (POLLHUP | POLLERR)) != 0) {
                    mark_remove(remove_clients, target.index);
                    continue;
                }
                if ((revents & POLLIN) == 0) continue;

                const ssize_t bytes_read = client_read(client.conn, read_buffer, sizeof(read_buffer));
                if (bytes_read == 0) {
                    mark_remove(remove_clients, target.index);
                    continue;
                }
                if (bytes_read < 0) {
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    mark_remove(remove_clients, target.index);
                    continue;
                }

                client.recv_buffer.append(read_buffer, static_cast<std::size_t>(bytes_read));
                while (true) {
                    const std::size_t nl = client.recv_buffer.find('\n');
                    if (nl == std::string::npos) break;
                    const std::string line = trim_copy(client.recv_buffer.substr(0, nl));
                    client.recv_buffer.erase(0, nl + 1);
                    if (line.empty()) continue;

                    if (line.rfind("SUB_POS|", 0) == 0) {
                        const std::string requested_id = normalize_object_id_token(line.substr(8));
                        if (requested_id.empty()) continue;

                        std::cout << "[main.cpp] [Position] SUB_POS 수신: ip="
                                  << client.conn.ip << ", object_id=" << requested_id
                                  << std::endl;
                        client.active_object_id = requested_id;
                        switch_esp_track_target(requested_id);
                        continue;
                    }

                    if (line.rfind("UNSUB_POS|", 0) == 0) {
                        const std::string requested_id = normalize_object_id_token(line.substr(10));
                        if (requested_id.empty()) continue;
                        std::cout << "[main.cpp] [Position] UNSUB_POS 수신: ip="
                                  << client.conn.ip << ", object_id=" << requested_id
                                  << std::endl;
                        if (client.active_object_id == requested_id) {
                            client.active_object_id.clear();
                            clear_esp_track_target(requested_id, "UNSUB");
                        }
                        continue;
                    }
                }

                if (client.recv_buffer.size() > kMaxRecvBuffer) {
                    client.recv_buffer.clear();
                }
            }
        }

        const auto erase_removed_clients = [&]() {
            unique_descending(remove_clients);
            for (const std::size_t idx : remove_clients) {
                if (idx >= clients.size()) continue;
                const std::string disconnected_ip = clients[idx].conn.ip;
                const char* reason = clients[idx].conn.kind == TransportKind::Plain
                                         ? "position_plain_disconnect"
                                         : "position_tls_disconnect";
                close_client(clients[idx].conn);
                clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(idx));
                schedule_deauth(disconnected_ip, reason);
            }
            remove_clients.clear();
        };

        erase_removed_clients();
        reconcile_esp_track_target();

        const auto now = std::chrono::steady_clock::now();
        const auto esp_stale_limit =
            std::chrono::seconds(static_cast<long long>(sec_cfg.position_stale_seconds));
        const auto obj_stale_limit = std::chrono::seconds(1);
        const auto obj_min_send_interval =
            std::chrono::milliseconds(std::max(1, sec_cfg.position_min_send_ms));

        esp_manager.expireFraudTrackIfStale(esp_stale_limit);

        if (!esp_active_object_id.empty()) {
            AnalyticsProcessor::ObjectPositionSnapshot snapshot;
            const bool has_snapshot =
                analytics.getObjectPositionSnapshot(esp_active_object_id, snapshot);
            const bool is_stale = (!has_snapshot) || ((now - snapshot.updated_at) > esp_stale_limit);

            if (is_stale) {
                clear_esp_track_target(esp_active_object_id, "STALE");
            } else if (!esp_has_last_sent || !snapshots_equal(esp_last_sent, snapshot)) {
                try_publish_esp_track_pos(esp_active_object_id);
            }
        }

        if (sec_cfg.esp_test_track_pos_enable &&
            !sec_cfg.esp_test_track_pos_object_id.empty() &&
            now >= esp_test_track_pos_next_at) {
            EspManager::TrackPosPayload test_payload;
            test_payload.object_id = sec_cfg.esp_test_track_pos_object_id;
            test_payload.left = 1.0f;
            test_payload.top = 1.0f;
            test_payload.right = 1.0f;
            test_payload.bottom = 1.0f;
            test_payload.x = 1.0f;
            test_payload.y = 1.0f;
            test_payload.tag_time = "1";
            const bool sent = esp_manager.publishTrackPos(test_payload);
            std::cout << "[main.cpp] [ESP_TEST] TRACK_POS(" << test_payload.object_id
                      << ") " << (sent ? "sent" : "skipped(no client/send fail)")
                      << std::endl;
            esp_test_track_pos_next_at = now + esp_test_track_pos_interval;
        }

        const auto broadcast_obj_line = [&](const std::string& line) -> bool {
            if (line.empty()) return true;

            bool delivered = false;
            for (std::size_t idx = 0; idx < clients.size(); ++idx) {
                if (!client_send_line(clients[idx].conn, line)) {
                    mark_remove(remove_clients, idx);
                    continue;
                }
                delivered = true;
            }
            return delivered;
        };

        analytics.getAllObjectSnapshots(obj_snapshots);
        obj_ids_this_tick.clear();
        obj_ids_this_tick.reserve(obj_snapshots.size());

        for (const auto& snapshot : obj_snapshots) {
            const std::string& object_id = snapshot.object_id;
            if (object_id.empty()) continue;

            obj_ids_this_tick.insert(object_id);
            const bool is_stale = (now - snapshot.updated_at) > obj_stale_limit;

            if (is_stale) {
                const auto sent_it = obj_last_sent.find(object_id);
                if (sent_it == obj_last_sent.end()) continue;

                const std::string end_line = format_obj_end_line(object_id, "STALE");
                broadcast_obj_line(end_line);
                obj_last_sent.erase(object_id);
                continue;
            }

            const auto sent_it = obj_last_sent.find(object_id);
            if (sent_it != obj_last_sent.end() &&
                sent_it->second.updated_at == snapshot.updated_at &&
                sent_it->second.is_fraud == snapshot.is_fraud) {
                continue;
            }
            if (sent_it != obj_last_sent.end() &&
                (now - sent_it->second.sent_at) < obj_min_send_interval) {
                continue;
            }

            const std::string obj_line = format_obj_pos_line(snapshot);
            if (broadcast_obj_line(obj_line)) {
                ObjLastSentState sent_state;
                sent_state.updated_at = snapshot.updated_at;
                sent_state.sent_at = now;
                sent_state.is_fraud = snapshot.is_fraud;
                obj_last_sent[object_id] = sent_state;
            }
        }

        for (auto it = obj_last_sent.begin(); it != obj_last_sent.end();) {
            if (obj_ids_this_tick.find(it->first) == obj_ids_this_tick.end()) {
                const std::string end_line = format_obj_end_line(it->first, "STALE");
                broadcast_obj_line(end_line);
                it = obj_last_sent.erase(it);
            } else {
                ++it;
            }
        }

        erase_removed_clients();
        run_pending_deauth();
    }

    for (auto& client : clients) {
        close_client(client.conn);
    }
    close_listener_bundle(listeners);
    std::cout << "[main.cpp] [Position] 스트림 서비스 스레드 종료." << std::endl;
}

}  // namespace app_services_impl
