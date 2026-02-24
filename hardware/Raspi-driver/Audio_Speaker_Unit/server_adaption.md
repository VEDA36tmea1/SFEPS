# 서버 연동 (Server Adaptation)

SFEPS 서버(`server/`)에서 Audio_Speaker_Unit의 링 버퍼·ALSA 재생 모듈을 사용해 음성 수신·재생하는 방법을 정리한 문서입니다.

---

## 개요

클라이언트가 **5556 포트**로 보내는 **16 kHz, 모노, S16_LE RAW PCM**을 서버가 TCP로 수신한 뒤, 링 버퍼에 넣고 별도 재생 스레드가 ALSA로 스트리밍 재생합니다.  
기존 `aplay` 파이프 방식 대신, 본 디렉터리(Audio_Speaker_Unit)와 동일한 방식으로 동작합니다.

---

## 1. server/CMakeLists.txt 수정

### 1.1 Audio_Speaker_Unit 경로

- **경로:** `../hardware/Raspi-driver/Audio_Speaker_Unit` (서버 소스 기준 상대 경로)
- CMake 변수로 지정해 include·소스 경로에 사용합니다.

```cmake
set(AUDIO_SPEAKER_UNIT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../hardware/Raspi-driver/Audio_Speaker_Unit)
```

### 1.2 추가 소스

- `audio_ring_buffer.cpp`, `audio_playback.cpp` 를 `smart_server` 타깃에 포함합니다.
- `main.cpp` 는 포함하지 않습니다 (서버 전용 진입점 사용).

```cmake
add_executable(smart_server
    # ... 기존 소스 ...
    ${AUDIO_SPEAKER_UNIT_DIR}/src/audio_ring_buffer.cpp
    ${AUDIO_SPEAKER_UNIT_DIR}/src/audio_playback.cpp
)
```

### 1.3 Include 경로

- `Audio_Speaker_Unit/include` 를 추가해 `audio_common.h`, `audio_ring_buffer.h`, `audio_playback.h` 를 찾을 수 있게 합니다.

```cmake
target_include_directories(smart_server PRIVATE
    # ... 기존 include ...
    ${AUDIO_SPEAKER_UNIT_DIR}/include
)
```

### 1.4 링크

- ALSA 오디오 재생을 위해 `asound` 를 링크합니다.

```cmake
target_link_libraries(smart_server PRIVATE
    # ... 기존 라이브러리 ...
    asound
)
```

---

## 2. server/src/main.cpp 수정

### 2.1 인클루드

다음 헤더를 추가합니다.

```cpp
#include "audio_common.h"
#include "audio_ring_buffer.h"
#include "audio_playback.h"
```

### 2.2 run_audio_receiver() 동작

음성 수신 스레드에서 다음 순서로 동작합니다.

| 단계 | 내용 |
|------|------|
| 1 | `AudioRingBuffer` 생성 (용량: 약 1초 분량, `AUDIO_SAMPLE_RATE * AUDIO_FRAME_BYTES * 1`) |
| 2 | `AudioPlayback` 생성 후 `playback.start()` 로 재생 스레드 기동 |
| 3 | `AUDIO_PORT(5556)` 에서 TCP listen → `accept()` |
| 4 | 수신한 RAW PCM을 `ring.push(buf, bytes)` 로 링 버퍼에 적재 |
| 5 | 클라이언트 연결 종료 시 해당 소켓만 `close()`, 링 버퍼·재생 스레드는 유지 (다음 연결 대기) |
| 6 | 전역 `g_running` 이 false 가 되면 `ring.stop()` → `playback.stop()` → `close(server_fd)` 로 정리 |

- 포맷: **16 kHz, 모노, S16_LE** (audio_common.h 상수 사용)
- 수신 버퍼: 4096 바이트 단위로 `read` 후 링 버퍼에 push
- 서버 종료 시에만 링 버퍼와 재생 스레드를 멈추므로, 여러 클라이언트가 순차적으로 연결해도 동일 링 버퍼·재생 스레드를 재사용합니다.

---

## 3. 데이터 흐름 요약

```
[클라이언트] --TCP:5556, RAW PCM--> [서버 run_audio_receiver]
                                        |
                                        v
                                  AudioRingBuffer (ring)
                                        |
                                        v
                                  AudioPlayback (별도 스레드)
                                        |
                                        v
                                  ALSA (default, S16_LE, 16kHz, mono)
```

---

## 4. 참고

- 오디오 포맷 상수는 `include/audio_common.h` 에 정의되어 있으며, 서버·클라이언트·Audio_Speaker_Unit 테스트가 동일 값을 사용합니다.
- 서버는 Audio_Speaker_Unit의 **RAW 모드**와 동일한 수신·재생 방식을 사용하며, MP3 모드는 사용하지 않습니다.
