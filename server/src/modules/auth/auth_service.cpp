#include "app_services_impl.h"

#include <poll.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <vector>

#include "alert.h"
#include "auth.h"
#include "log.h"
#include "service_shared.h"
#include "transport_utils.h"

namespace app_services_impl {

void run_login_auth_impl(std::atomic<bool>& running,
                         const RuntimeConfig& cfg,
                         const SecurityRuntimeOptions& sec_cfg) {
    using namespace app_services_shared;
    using namespace app_services_transport;

    struct AttemptState {
        int fail_count = 0;
        std::chrono::steady_clock::time_point lock_until =
            std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point last_seen =
            std::chrono::steady_clock::time_point::min();
    };

    constexpr int kMaxFail = 5;
    constexpr int kCleanupInterval = 100;
    const auto kLockDuration = std::chrono::seconds(30);
    const auto kStaleRetention = std::chrono::minutes(10);

    Authenticator auth(cfg.db_host.c_str(),
                       cfg.db_user.c_str(),
                       cfg.db_pass.c_str(),
                       cfg.db_name_analytics.c_str());
    if (!auth.connect()) {
        std::cerr << "[main.cpp] [Fatal] Auth DB 연결 실패(fail-closed)." << std::endl;
        running = false;
        return;
    }

    DBLogger auth_logger(cfg.db_host.c_str(),
                         cfg.db_user.c_str(),
                         cfg.db_pass.c_str(),
                         cfg.db_name_analytics.c_str());
    if (!auth_logger.connect()) {
        std::cerr << "[main.cpp] [Warn] Auth logger DB 연결 실패. "
                  << "Login service will continue without auth DB log writes." << std::endl;
    }

    std::cout << "[AuthFlow][4] run_login_auth 시작. "
              << "plaintext=" << (sec_cfg.app_plaintext_enable ? "on" : "off")
              << ", tls=" << (sec_cfg.app_tls_enable ? "on" : "off")
              << ", auth_tls_port=" << sec_cfg.auth_tls_port << std::endl;

    ListenerBundle listeners;
    if (!start_listener_bundle(listeners,
                               sec_cfg,
                               kAuthPort,
                               sec_cfg.auth_tls_port,
                               "Auth",
                               "AuthTLS",
                               true,
                               true)) {
        running = false;
        return;
    }

    if (sec_cfg.app_plaintext_enable && listeners.plain_server_fd >= 0) {
        std::cout << "[AuthFlow][4] auth plaintext listener ready on " << kAuthPort << std::endl;
    }
    if (sec_cfg.app_tls_enable && listeners.tls_server.listen_fd >= 0) {
        std::cout << "[AuthFlow][4] auth TLS listener ready on " << sec_cfg.auth_tls_port
                  << std::endl;
    }

    std::unordered_map<std::string, AttemptState> attempts;
    int request_counter = 0;

    auto evaluate_auth = [&](const std::string& data,
                             const std::string& client_ip,
                             bool oversized,
                             std::string& user,
                             bool& success) {
        user = "Unknown";
        success = false;
        bool valid_format = false;

        if (!oversized) {
            const std::size_t sep = data.find(':');
            if (sep != std::string::npos) {
                user = trim_copy(data.substr(0, sep));
                const std::string pass = trim_copy(data.substr(sep + 1));
                if (!user.empty() && !pass.empty()) {
                    valid_format = true;
                    const std::string login_key = normalize_login_key(user) + "|" + client_ip;
                    const auto now = std::chrono::steady_clock::now();
                    AttemptState& state = attempts[login_key];
                    state.last_seen = now;

                    if (state.lock_until > now) {
                        success = false;
                    } else {
                        success = auth.authenticate(user, pass);
                        if (success) {
                            state.fail_count = 0;
                            state.lock_until = std::chrono::steady_clock::time_point::min();
                        } else {
                            state.fail_count += 1;
                            if (state.fail_count >= kMaxFail) {
                                state.fail_count = 0;
                                state.lock_until = now + kLockDuration;
                            }
                        }
                    }
                }
            }
        }

        if (oversized) {
            success = false;
        } else if (!valid_format) {
            success = false;
        }

        if (++request_counter % kCleanupInterval == 0) {
            const auto now = std::chrono::steady_clock::now();
            for (auto it = attempts.begin(); it != attempts.end();) {
                const bool is_locked = it->second.lock_until > now;
                const bool is_stale =
                    it->second.last_seen != std::chrono::steady_clock::time_point::min() &&
                    ((now - it->second.last_seen) > kStaleRetention);
                if (!is_locked && it->second.fail_count == 0 && is_stale) {
                    it = attempts.erase(it);
                } else {
                    ++it;
                }
            }
        }
    };

    while (running.load()) {
        std::vector<pollfd> pfds;
        append_listener_pollfds(listeners, pfds);
        if (pfds.empty()) break;

        const int poll_ret = poll(pfds.data(), pfds.size(), 1000);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Auth] poll() 실패: " << std::strerror(errno) << std::endl;
            break;
        }
        if (poll_ret == 0) continue;

        for (const pollfd& pfd : pfds) {
            if ((pfd.revents & POLLIN) == 0) continue;

            TransportKind kind;
            if (!resolve_listener_kind(listeners, pfd.fd, kind)) continue;

            AcceptedClient client;
            if (!accept_client(listeners, kind, sec_cfg.auth_allow_ips, "Auth", client)) {
                if (!running.load()) break;
                continue;
            }

            std::cout << "[AuthFlow][4] " << (kind == TransportKind::Plain ? "plain" : "TLS")
                      << " auth client accepted: ip=" << client.ip << ", fd=" << client.fd
                      << std::endl;

            apply_read_timeout(client, sec_cfg.socket_read_timeout_ms);

            std::vector<char> buf(sec_cfg.auth_max_bytes + 1, 0);
            const ssize_t bytes_read = client_read(client, buf.data(), buf.size());
            if (bytes_read <= 0) {
                if (bytes_read < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    std::cerr << "[Auth] " << transport_name(kind)
                              << " read() 실패: " << std::strerror(errno) << std::endl;
                }
                close_client(client);
                continue;
            }

            const bool oversized = static_cast<std::size_t>(bytes_read) > sec_cfg.auth_max_bytes;
            const std::size_t data_len =
                oversized ? sec_cfg.auth_max_bytes : static_cast<std::size_t>(bytes_read);
            const std::string data(buf.data(), data_len);

            std::string user;
            bool success = false;
            evaluate_auth(data, client.ip, oversized, user, success);
            std::cout << "[main.cpp] [Auth] " << transport_name(kind)
                      << " login attempt result: ip=" << client.ip << ", user=" << user
                      << ", result=" << (success ? "PASS" : "FAIL") << std::endl;

            if (oversized) {
                std::cout << "[main.cpp] [Auth] " << transport_name(kind)
                          << " payload 거부: 초과 SFEPS_AUTH_MAX_BYTES="
                          << sec_cfg.auth_max_bytes << " (ip=" << client.ip << ")"
                          << std::endl;
            }

            const char* resp = success ? "PASS" : "FAIL";
            client_send_all(client, resp, 4);

            if (success) {
                mark_ip_authenticated(client.ip);
                std::cout << "[main.cpp] [Auth] " << transport_name(kind)
                          << " login session registered: ip=" << client.ip
                          << ", user=" << user << std::endl;
                send_alert_to_clients("TEST|LOGIN_OK|" + user + "\n");
            }

            auth_logger.enqueueLogin(user, client.ip, success);
            close_client(client);
        }
    }

    close_listener_bundle(listeners);
    std::cout << "[main.cpp] [Auth] auth 스레드 종료." << std::endl;
}

}  // namespace app_services_impl
