#pragma once

// 공통 오디오 포맷 상수 (클라이언트/서버/테스트 공용)

// 샘플레이트: 16 kHz (음성용)
constexpr int AUDIO_SAMPLE_RATE = 16000;

// 채널 수: 1 (모노)
constexpr int AUDIO_CHANNELS = 1;

// 샘플 포맷: S16_LE (16비트, 리틀엔디안, signed)
// libasound에서는 SND_PCM_FORMAT_S16_LE 를 사용

// Int16 1샘플 = 2바이트
constexpr int AUDIO_SAMPLE_BYTES = 2;

// 프레임당 바이트 수 (모노에서는 샘플과 동일)
constexpr int AUDIO_FRAME_BYTES = AUDIO_SAMPLE_BYTES * AUDIO_CHANNELS;
