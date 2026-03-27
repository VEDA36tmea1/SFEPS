#include "audio_playback.h"

#include <iostream>
#include <vector>

AudioPlayback::AudioPlayback(AudioRingBuffer& ring)
    : ring_(ring)
{
}

AudioPlayback::~AudioPlayback()
{
    stop();
}

bool AudioPlayback::initPcm()
{
    int err = snd_pcm_open(&pcm_handle_, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        std::cerr << "[AudioPlayback] snd_pcm_open failed: " << snd_strerror(err) << std::endl;
        pcm_handle_ = nullptr;
        return false;
    }

    // 간단 설정: interleaved, S16_LE, mono, 16kHz, soft-resample 허용, 지연 기본값
    err = snd_pcm_set_params(
        pcm_handle_,
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_ACCESS_RW_INTERLEAVED,
        AUDIO_CHANNELS,
        AUDIO_SAMPLE_RATE,
        1,              // soft_resample
        50000           // latency in us (50ms 정도) - 이후 튜닝 가능
    );
    if (err < 0) {
        std::cerr << "[AudioPlayback] snd_pcm_set_params failed: " << snd_strerror(err) << std::endl;
        closePcm();
        return false;
    }

    // period/buffer 크기를 상세 제어하려면 hw_params를 별도로 사용할 수 있지만,
    // 우선 set_params 기반으로 시작하고 나중에 튜닝.
    snd_pcm_uframes_t buf_size = 0;
    snd_pcm_uframes_t period_size = 0;
    snd_pcm_get_params(pcm_handle_, &buf_size, &period_size);
    period_frames_ = static_cast<unsigned int>(period_size > 0 ? period_size : 256);

    std::cout << "[AudioPlayback] PCM initialized. buffer_frames=" << buf_size
              << " period_frames=" << period_frames_ << std::endl;
    return true;
}

void AudioPlayback::closePcm()
{
    if (pcm_handle_) {
        snd_pcm_close(pcm_handle_);
        pcm_handle_ = nullptr;
    }
}

bool AudioPlayback::start()
{
    if (running_) return true;
    if (!initPcm()) return false;
    running_ = true;
    thread_ = std::thread(&AudioPlayback::playbackThreadFunc, this);
    return true;
}

void AudioPlayback::stop()
{
    if (!running_) return;
    running_ = false;
    ring_.stop(); // pop 대기 중인 스레드 깨우기
    if (thread_.joinable()) thread_.join();
    closePcm();
}

void AudioPlayback::playbackThreadFunc()
{
    if (!pcm_handle_) return;

    const std::size_t bytes_per_period = period_frames_ * AUDIO_FRAME_BYTES;
    std::vector<char> buffer(bytes_per_period);

    while (running_) {
        // 링 버퍼에서 최대 bytes_per_period 만큼 pop
        std::size_t popped = ring_.pop(buffer.data(), bytes_per_period);
        if (popped == 0) {
            if (!running_ || ring_.stopped()) break;
            continue;
        }

        // popped 바이트 수를 프레임 기준으로 환산
        snd_pcm_sframes_t frames = static_cast<snd_pcm_sframes_t>(popped / AUDIO_FRAME_BYTES);
        if (frames <= 0) continue;

        char* data = buffer.data();
        while (frames > 0 && running_) {
            snd_pcm_sframes_t written = snd_pcm_writei(pcm_handle_, data, frames);
            if (written == -EPIPE) {
                snd_pcm_prepare(pcm_handle_);
                continue;
            } else if (written < 0) {
                std::cerr << "[AudioPlayback] snd_pcm_writei error: "
                          << snd_strerror(written) << std::endl;
                break;
            }

            frames -= written;
            data   += written * AUDIO_FRAME_BYTES;
        }
    }

    // 재생 중이던 버퍼를 비우고 종료
    snd_pcm_drain(pcm_handle_);
}
