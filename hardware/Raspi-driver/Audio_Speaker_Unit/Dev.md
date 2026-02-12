# Audio_Speaker_Unit 개발 노트

## 1. 현재까지 구현된 것 요약

- **audio_common.h**
  - `AUDIO_SAMPLE_RATE = 16000` (16 kHz)
  - `AUDIO_CHANNELS = 1` (모노)
  - `AUDIO_FRAME_BYTES = 2` 등 공통 상수 정의 (Int16, S16_LE 기준).

- **audio_ring_buffer.h / audio_ring_buffer.cpp**
  - `AudioRingBuffer` 클래스:
    - 내부 구조: `std::vector<char>` + `std::mutex` + `std::condition_variable`
    - `push()` : 버퍼가 꽉 차면 공간이 생길 때까지 block 한 뒤 데이터 push
    - `pop()`  : 버퍼가 비어 있으면 데이터가 들어올 때까지 block 한 뒤 pop
    - `stop()` : 종료 플래그 설정 + 대기 중인 스레드 깨우기

- **audio_playback.h / audio_playback.cpp** (libasound 기반 Playback Thread)
  - `AudioPlayback` 클래스:
    - `initPcm()` 에서 `snd_pcm_open("default")` + `snd_pcm_set_params(...)`
      - 포맷: `SND_PCM_FORMAT_S16_LE`, 채널: 1ch, 샘플레이트: 16000Hz
    - `start()` : ALSA 초기화 후 재생 스레드 시작
    - `playbackThreadFunc()` :
      - `ring.pop()` 으로 period 단위 데이터 읽어서 `snd_pcm_writei()` 로 계속 출력
      - `-EPIPE`(XRUN) 발생 시 `snd_pcm_prepare()` 로 복구 후 재시도
    - `stop()` : `running_ = false`, `ring.stop()`, 스레드 join + `snd_pcm_close()`

- **main.cpp** (테스트용 바이너리)
  - 실행 흐름:
    1. `AudioRingBuffer` 생성
    2. `AudioPlayback` 시작 (Playback Thread + ALSA 초기화)
    3. `stdin` 에서 RAW PCM(S16_LE, 16kHz, mono)을 읽어서 `ring.push()`
    4. 링 버퍼 → Playback Thread → 오디오 잭으로 **바로 재생**

  - 라즈베리 파이에서 사용 예시:
    ```bash
    cd ~/SFEPS/hardware/Raspi-driver/Audio_Speaker_Unit
    g++ -o audio_speaker_test main.cpp audio_ring_buffer.cpp audio_playback.cpp -lasound -std=c++17

    # 예: 미리 녹음한 RAW 파일을 재생
    cat test.raw | ./audio_speaker_test
    ```

## 2. 다음 단계

- 이 테스트 바이너리로 **libasound + 링 버퍼 + Playback Thread 구조**를 라즈베리 파이에서 검증.
- 레이턴시/안정성이 만족스러우면:
  - `audio_common.h`, `audio_ring_buffer.*`, `audio_playback.*` 를 `server/` 로 옮겨
  - `run_audio_receiver()` 대신 **링 버퍼 push + Playback 모듈**을 사용하는 구조로 통합 예정.

