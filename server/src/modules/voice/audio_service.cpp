#include "app_services_impl.h"

#include <poll.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <vector>

#include "audio_common.h"
#include "audio_playback.h"
#include "audio_ring_buffer.h"
#include "service_shared.h"
#include "transport_utils.h"

namespace app_services_impl {

void run_audio_receiver_impl(std::atomic<bool>& running, const SecurityRuntimeOptions& sec_cfg) {
    using namespace app_services_shared;
    using namespace app_services_transport;

    constexpr std::size_t kBufferSize = 4096;
    const std::size_t ring_capacity_bytes =
        static_cast<std::size_t>(AUDIO_SAMPLE_RATE * AUDIO_FRAME_BYTES);

    AudioRingBuffer ring(ring_capacity_bytes);
    AudioPlayback playback(ring);
    if (!playback.start()) {
        std::cerr << "[Audio] Failed to start AudioPlayback" << std::endl;
        return;
    }

    ListenerBundle listeners;
    if (!start_listener_bundle(listeners,
                               sec_cfg,
                               kAudioPort,
                               sec_cfg.audio_tls_port,
                               "Audio",
                               "AudioTLS",
                               true,
                               true)) {
        ring.stop();
        playback.stop();
        running = false;
        return;
    }

    char buf[kBufferSize];
    while (running.load()) {
        std::vector<pollfd> pfds;
        append_listener_pollfds(listeners, pfds);
        if (pfds.empty()) break;

        const int poll_ret = poll(pfds.data(), pfds.size(), 1000);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Audio] poll() 실패: " << std::strerror(errno) << std::endl;
            break;
        }
        if (poll_ret == 0) continue;

        for (const pollfd& pfd : pfds) {
            if ((pfd.revents & POLLIN) == 0) continue;

            TransportKind kind;
            if (!resolve_listener_kind(listeners, pfd.fd, kind)) continue;

            AcceptedClient client;
            if (!accept_client(listeners, kind, sec_cfg.audio_allow_ips, "Audio", client)) {
                if (!running.load()) break;
                continue;
            }

            apply_read_timeout(client, sec_cfg.socket_read_timeout_ms);

            bool oversize = false;
            std::size_t total_bytes = 0;
            while (running.load()) {
                const ssize_t bytes_read = client_read(client, buf, sizeof(buf));
                if (bytes_read > 0) {
                    const std::size_t chunk = static_cast<std::size_t>(bytes_read);
                    if (total_bytes + chunk > sec_cfg.audio_max_bytes) {
                        oversize = true;
                        break;
                    }
                    ring.push(buf, chunk);
                    total_bytes += chunk;
                    continue;
                }

                if (bytes_read == 0) break;
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                std::cerr << "[Audio] " << transport_name(kind)
                          << " read() 실패: " << std::strerror(errno) << std::endl;
                break;
            }

            if (oversize) {
                std::cout << "[main.cpp] [Audio] " << transport_name(kind)
                          << " payload 거부: 초과 SFEPS_AUDIO_MAX_BYTES="
                          << sec_cfg.audio_max_bytes << " (ip=" << client.ip << ")"
                          << std::endl;
            }

            close_client(client);
        }
    }

    close_listener_bundle(listeners);
    ring.stop();
    playback.stop();
    std::cout << "[main.cpp] [Audio] 수신 스레드 종료." << std::endl;
}

}  // namespace app_services_impl
