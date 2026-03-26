#include "app_services_impl.h"

#include <poll.h>

#include <cmath>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <vector>

#include "audio_common.h"
#include "audio_playback.h"
#include "audio_ring_buffer.h"
#include "service_shared.h"
#include "transport_utils.h"

namespace app_services_impl {
namespace {

std::mutex g_local_audio_ring_mutex;
AudioRingBuffer* g_local_audio_ring = nullptr;
std::atomic<int> g_active_audio_input_clients {0};

std::vector<int16_t> make_rfid_tag_tone() {
    constexpr double kPi = 3.14159265358979323846;
    constexpr int kToneFreqHz = 2400;
    constexpr int kToneMs = 120;
    constexpr int kSilenceMs = 45;
    constexpr int kRepeatCount = 1;
    constexpr double kAmplitude = 0.22;
    constexpr int kRampMs = 8;

    const int tone_samples = (AUDIO_SAMPLE_RATE * kToneMs) / 1000;
    const int silence_samples = (AUDIO_SAMPLE_RATE * kSilenceMs) / 1000;
    const int ramp_samples = std::max(1, (AUDIO_SAMPLE_RATE * kRampMs) / 1000);
    std::vector<int16_t> pcm;
    pcm.reserve((tone_samples + silence_samples) * kRepeatCount);

    for (int repeat = 0; repeat < kRepeatCount; ++repeat) {
        for (int i = 0; i < tone_samples; ++i) {
            double gain = 1.0;
            if (i < ramp_samples) {
                gain = static_cast<double>(i) / ramp_samples;
            } else if (i >= tone_samples - ramp_samples) {
                gain = static_cast<double>(tone_samples - i) / ramp_samples;
            }
            const double t = static_cast<double>(i) / AUDIO_SAMPLE_RATE;
            const double sample =
                std::sin(2.0 * kPi * kToneFreqHz * t) * kAmplitude * gain * 32767.0;
            pcm.push_back(static_cast<int16_t>(sample));
        }
        for (int i = 0; i < silence_samples; ++i) {
            pcm.push_back(0);
        }
    }
    return pcm;
}

}  // namespace

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
    {
        std::lock_guard<std::mutex> lock(g_local_audio_ring_mutex);
        g_local_audio_ring = &ring;
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
        {
            std::lock_guard<std::mutex> lock(g_local_audio_ring_mutex);
            g_local_audio_ring = nullptr;
        }
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

            g_active_audio_input_clients.fetch_add(1, std::memory_order_relaxed);

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
            g_active_audio_input_clients.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    close_listener_bundle(listeners);
    {
        std::lock_guard<std::mutex> lock(g_local_audio_ring_mutex);
        g_local_audio_ring = nullptr;
    }
    ring.stop();
    playback.stop();
}

void play_local_rfid_tag_tone_impl() {
    if (g_active_audio_input_clients.load(std::memory_order_relaxed) > 0) {
        return;
    }

    AudioRingBuffer* ring = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_local_audio_ring_mutex);
        ring = g_local_audio_ring;
    }
    if (ring == nullptr) return;

    static const std::vector<int16_t> tone_pcm = make_rfid_tag_tone();
    ring->push(reinterpret_cast<const char*>(tone_pcm.data()),
               tone_pcm.size() * sizeof(int16_t));
}

}  // namespace app_services_impl
