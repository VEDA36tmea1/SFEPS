#pragma once

#include <atomic>
#include <thread>
#include <alsa/asoundlib.h>

#include "audio_common.h"
#include "audio_ring_buffer.h"

// ALSA 기반 재생 스레드를 관리하는 간단한 헬퍼 클래스
class AudioPlayback {
public:
    explicit AudioPlayback(AudioRingBuffer& ring);
    ~AudioPlayback();

    // 재생 스레드 시작
    bool start();

    // 재생 스레드 종료 및 ALSA 자원 정리
    void stop();

private:
    void playbackThreadFunc();
    bool initPcm();
    void closePcm();

    AudioRingBuffer& ring_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    snd_pcm_t* pcm_handle_ = nullptr;
    unsigned int period_frames_ = 0;
};

