#include "app_services_impl.h"

#include <poll.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <vector>

#include "alert.h"
#include "service_shared.h"
#include "transport_utils.h"

namespace app_services_impl {

void run_fraud_notifier_impl(std::atomic<bool>& running, const SecurityRuntimeOptions& sec_cfg) {
    using namespace app_services_shared;
    using namespace app_services_transport;

    ListenerBundle listeners;
    if (!start_listener_bundle(listeners,
                               sec_cfg,
                               kAlertPort,
                               sec_cfg.alert_tls_port,
                               "Alert",
                               "AlertTLS",
                               true,
                               true)) {
        running = false;
        return;
    }

    while (running.load()) {
        std::vector<pollfd> pfds;
        append_listener_pollfds(listeners, pfds);
        if (pfds.empty()) break;

        const int poll_ret = poll(pfds.data(), pfds.size(), 1000);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Alert] poll() 실패: " << std::strerror(errno) << std::endl;
            break;
        }
        if (poll_ret == 0) continue;

        for (const pollfd& pfd : pfds) {
            if ((pfd.revents & POLLIN) == 0) continue;

            TransportKind kind;
            if (!resolve_listener_kind(listeners, pfd.fd, kind)) continue;

            AcceptedClient client;
            if (!accept_client(listeners, kind, sec_cfg.alert_allow_ips, "Alert", client)) {
                if (!running.load()) break;
                continue;
            }

            apply_read_timeout(client, sec_cfg.socket_read_timeout_ms);

            if (alert_client_count() >= sec_cfg.alert_max_clients) {
                std::cout << "[main.cpp] [Alert] " << transport_name(kind)
                          << " 연결 거부: 최대 클라이언트 수 도달 ("
                          << sec_cfg.alert_max_clients << ")" << std::endl;
                close_client(client);
                continue;
            }

            if (kind == TransportKind::Plain) {
                add_alert_plain_client(client.fd, client.ip);
                std::cout << "[main.cpp] [Alert] plain 클라이언트 연결됨: " << client.ip << ":"
                          << client.port << " (fd=" << client.fd << ")" << std::endl;
                client.fd = -1;
            } else {
                add_alert_tls_client(std::move(client.tls_conn), client.ip);
                std::cout << "[main.cpp] [Alert] TLS 클라이언트 연결됨: " << client.ip << ":"
                          << client.port << " (fd=" << client.fd << ")" << std::endl;
                client.fd = -1;
            }
        }
    }

    close_listener_bundle(listeners);
    close_alert_client_connections();
    std::cout << "[main.cpp] [Alert] 알림 스레드 종료." << std::endl;
}

}  // namespace app_services_impl
