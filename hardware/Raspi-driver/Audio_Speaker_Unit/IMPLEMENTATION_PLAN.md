# Audio_Speaker_Unit – libasound + 링 버퍼 + Playback Thread 구현 계획

라즈베리 파이에서 **“매니저가 마이크 버튼 누르면 스피커에서 바로 소리 나오는”** 구조를  
`Audio_Speaker_Unit` 폴더에서 먼저 완성한 뒤, 나중에 `server/` 쪽에 모듈로 붙이는 것을 목표로 합니다.

---

## 1. 전체 아키텍처

- **입력 쪽**: (초기엔 서버/네트워크 없이) 테스트용 RAW 파일 또는 간단한 TCP 수신으로 시작 가능  
- **코어 모듈 (이 폴더에서 구현)**:
  - **Receiver Thread (Producer)**: RAW PCM(S16_LE, 16kHz, mono)을 **링 버퍼에 push**
  - **Playback Thread (Consumer, libasound)**: 링 버퍼에서 프레임을 꺼내 `snd_pcm_writei()` 로 계속 출력
- **서버 통합 이후**:
  - `server/` 의 음성 수신 코드에서 **이 모듈의 “push()”만 호출**하도록 변경

---

## 2. 오디오 포맷 상수 (공통 규격)

**클라이언트(QAudioSource)와 동일하게 고정**:

- `AUDIO_SAMPLE_RATE = 16000`  (16 kHz)
- `AUDIO_CHANNELS = 1`         (mono)
- `AUDIO_FORMAT = SND_PCM_FORMAT_S16_LE`  (16-bit, little endian, signed)
- `FRAME_BYTES = 2 * AUDIO_CHANNELS`     (Int16 1샘플 = 2바이트)

이 값들은 나중에 서버 쪽과도 **동일하게 사용**할 것이므로,  
`audio_common.h` 또는 이 폴더의 공용 헤더에 한 번만 정의합니다.

---

## 3. 링 버퍼 설계 (Producer/Consumer)

### 3-1. 단위

- 바이트가 아니라 **프레임 단위**로 관리하는 것을 권장합니다.
  - frame = (샘플 1개 × 채널 수)
  - 실제 버퍼에는 `frame_count * FRAME_BYTES` 만큼의 바이트를 저장

### 3-2. 구조

- 간단한 시작안:
  - 내부 버퍼: `std::vector<char>` 또는 `std::deque<char>`  
  - 보호: `std::mutex + std::condition_variable`
  - API 예시:
    - `void push(const char* data, size_t bytes);`
    - `size_t pop(char* out, size_t max_bytes);`  (필요한 프레임 수만큼 꺼내기)

### 3-3. 정책

- 버퍼가 꽉 찰 때:
  - v1: 일단은 **push 측을 block** (condition_variable로 대기)
  - 나중에 필요하면:
    - “가장 오래된 데이터를 버리고 최신만 유지(drop oldest)” 같은 정책 추가

---

## 4. ALSA(PCM) 초기화 – Playback Thread

### 4-1. 초기화 흐름

Playback Thread 시작 시 **한 번만**:

1. `snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);`
2. `snd_pcm_set_params(...)` 또는 `snd_pcm_hw_params_*` 로:
   - format = `AUDIO_FORMAT`
   - rate   = `AUDIO_SAMPLE_RATE`
   - channels = `AUDIO_CHANNELS`
3. **latency 튜닝** (추정 값으로 시작):
   - period: 10~20 ms 정도
   - buffer: period 의 4배 정도 (40~80 ms 정도)

### 4-2. XRUN(underrun) 처리

- `snd_pcm_writei()` 가 `-EPIPE` 를 반환하면:
  - `snd_pcm_prepare(pcm);` 호출 후 다시 write 시도

---

## 5. Playback Thread 루프 (Consumer)

### 5-1. 목표

- 항상 “period 에 필요한 frame 수” 만큼 모아서 `snd_pcm_writei()` 로 출력
- 링 버퍼에 데이터가 부족하면:
  - 잠깐 대기(조건변수) 또는
  - 무음(0)으로 채워서 underrun 방지 (UX에 따라 결정)

### 5-2. 흐름

1. 필요 프레임 수 계산: `frames_per_period`
2. `frames_per_period * FRAME_BYTES` 만큼 링 버퍼에서 pop
3. 부족하면:
   - 대기 또는
   - 나머지를 `0`으로 패딩
4. `snd_pcm_writei(pcm, local_buffer, frames_per_period);`
5. `g_running` 플래그(또는 bool)로 종료 제어

---

## 6. Receiver Thread (Producer)

초기에는 **간단한 입력 소스**로 시작할 수 있습니다:

- 버전 1: 테스트용 RAW 파일을 읽어서 링 버퍼에 밀어넣기
- 버전 2: 지금 서버와 동일하게 TCP 수신(`accept` + `read`) 후, read() 결과를 바로 `push()` 로 전달

### 6-1. 주의점

- 링 버퍼에 push 할 때:
  - `bytes % FRAME_BYTES == 0` 이 되도록 맞추는 것이 이상적
  - 애매하면 남는 부분은 버리거나 다음 청크와 합쳐서 처리

---

## 7. 종료/상태 전파

- 공통 종료 플래그 (예: `std::atomic<bool> g_running`):
  - `false` 로 만들면:
    - Receiver Thread: 루프 종료
    - Playback Thread: 링 버퍼 비울 만큼만 재생하고 종료
- Playback Thread 종료 시:
  - `snd_pcm_drain()` 또는 `snd_pcm_drop()` 호출 후 `snd_pcm_close(pcm);`

---

## 8. 빌드/의존성 (이 폴더 전용)

- 패키지:
  - `sudo apt install libasound2-dev`
- 컴파일/링크 예시:
  - `g++ -o audio_speaker_test main.cpp audio_playback.cpp -lasound -std=c++17`
- (CMake를 도입한다면):
  - `target_link_libraries(audio_speaker_test asound)`

---

## 9. 파일 구성(권장)

- `Audio_Speaker_Unit/audio_common.h`
  - 오디오 포맷 상수 (`AUDIO_SAMPLE_RATE`, `AUDIO_CHANNELS`, `AUDIO_FORMAT`, `FRAME_BYTES`)
- `Audio_Speaker_Unit/audio_ring_buffer.h/.cpp`
  - 링 버퍼 구현 (mutex + condition_variable)
- `Audio_Speaker_Unit/audio_playback.h/.cpp`
  - ALSA 초기화 + Playback Thread (Consumer)
- `Audio_Speaker_Unit/main.cpp`
  - Receiver Thread (Producer) + `main()` (테스트용 바이너리)

---

## 10. 단계별 체크리스트

1. **공통 오디오 상수 정의** (`audio_common.h`)
2. **간단한 링 버퍼 구현** (`audio_ring_buffer.*`) – mutex/condvar 기반
3. **libasound Playback Thread 작성** (`audio_playback.*`)
4. **main.cpp 에서**:
   - Playback Thread 시작
   - 테스트 입력(파일 또는 TCP 수신) → 링 버퍼 `push()` 로 연결
5. 라즈베리 파이에서 **실제 스피커/헤드셋으로 레이턴시 테스트**
6. 충분히 만족스러우면:
   - 이 모듈들을 `server/` 로 옮겨서 `run_audio_receiver()` 대신 사용하도록 통합

