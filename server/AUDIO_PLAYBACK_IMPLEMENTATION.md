# 음성 수신 → 라즈베리 파이 오디오 잭 재생 구현 단계

음성 수신 스레드에서 TCP로 받은 데이터를 **바로** 라즈베리 파이 3.5mm 오디오 잭(스피커/헤드셋)으로 출력하기 위한 구현 단계 정리입니다.

---

## 현재 동작

- **AUDIO_PORT(5556)** 에서 클라이언트 연결 대기.
- 연결 시 클라이언트가 보내는 데이터를 **메모리 버퍼(`std::vector<char>`)에 수집**.
- 수신 완료 후 **ALSA(`aplay`)로 바로 재생** (파일 저장 없음).
- 재생 포맷: **16000 Hz, 모노, S16_LE** (클라이언트와 동일).

---

## 목표

- ✅ **구현 완료**: 수신한 음성 데이터를 **메모리 버퍼에서 바로** **기본 ALSA 디바이스(오디오 잭)** 로 재생.

---

## 전제: 오디오 포맷

재생하려면 **포맷이 고정**되어 있어야 합니다. 서버와 클라이언트가 같은 규격을 써야 합니다.

| 항목 | 권장 예시 | 비고 |
|------|-----------|------|
| **포맷** | RAW (헤더 없음) | 현재와 동일 |
| **샘플 포맷** | S16_LE (16비트 부호 있는 정수, 리틀 엔디안) | ALSA 기본 |
| **샘플레이트** | 16000 Hz 또는 44100 Hz | 음성: 16kHz, 음악: 44.1kHz |
| **채널** | 1 (모노) | 음성에 적합 |

문서/코드에 **“클라이언트는 16000 Hz, mono, S16_LE RAW 로 전송한다”** 처럼 명시하고, 서버는 이 값으로 재생하면 됩니다.

---

## 구현 단계

### Step 1: 오디오 포맷 상수 정의

- 서버 코드(또는 공용 헤더)에 다음을 정의해 두기.
  - `AUDIO_SAMPLE_RATE` (예: 16000)
  - `AUDIO_CHANNELS` (예: 1)
  - `AUDIO_FORMAT` (예: S16_LE → ALSA에서는 `SND_PCM_FORMAT_S16_LE`)
- 클라이언트(전송 측)도 동일한 값으로 녹음·인코딩하도록 문서화.

---

### Step 2: 재생 방식 선택

| 방식 | 장점 | 단점 |
|------|------|------|
| **A) `popen("aplay ...")`** | 구현 간단, ALSA 설정만 맞으면 동작 | 프로세스 생성 오버헤드, 재생 끝날 때까지 대기 가능 |
| **B) libasound (ALSA API)** | 프로세스 생성 없음, 버퍼/논블로킹 제어 가능 | 의존성(libasound2-dev), 코드량 증가 |

- **우선 구현**: A로 빠르게 동작 확인 후, 필요하면 B로 전환.
- **A 예시**: 수신 완료 후  
  `aplay -f S16_LE -r 16000 -c 1 -D default /path/to/voice_xxx.raw`  
  또는 수신 데이터를 파이프로 넘기기:  
  `aplay -f S16_LE -r 16000 -c 1 -` 에 stdin으로 전달.

---

### Step 3: 수신 루프에서 “저장 + 재생” 연결

**옵션 3-1: 수신 완료 후 한 번에 재생**

1. 지금처럼 수신 데이터를 모두 파일에 저장.
2. `close(client_fd)` 직후, 방금 쓴 파일 경로로 `aplay`(또는 ALSA API) 호출.
3. 재생이 끝날 때까지 대기(블로킹)하면, 그동안 다음 `accept()` 는 지연됨.  
   → “연결 하나씩 처리, 재생 끝난 뒤 다음 클라이언트”면 충분할 때 적합.

**옵션 3-2: 수신하면서 스트리밍 재생**

1. `accept()` 후, 수신 스레드에서 `read()` 할 때마다:
   - 파일에도 write (기존처럼 저장).
   - 동시에 재생용 버퍼(링 버퍼 등)에 넣고, **별도 재생 스레드**가 그 버퍼를 읽어 ALSA로 재생.
2. 지연을 줄일 수 있지만, 버퍼 크기·스레드 동기화·underrun 처리 필요.

**권장**: 먼저 **3-1(수신 완료 후 재생)** 으로 구현하고, 지연이 문제되면 3-2 검토.

---

### Step 4: 라즈베리 파이에서 재생 디바이스

- **ALSA 기본 디바이스**: `default` 또는 `plughw:0,0`.  
  라즈베리 파이에서는 보통 **3.5mm 오디오 잭이 default** 로 잡혀 있음.
- `aplay -L` / `aplay -l` 로 디바이스 목록 확인 가능.
- 코드에서는 `-D default` (또는 환경에 맞는 디바이스 이름) 사용하면 됨.

---

### Step 5: 의존성 및 빌드

- **A (aplay)**  
  - 실행 경로: `aplay` 가 PATH에 있어야 함 (일반적으로 `alsa-utils` 패키지).  
  - 라즈베리 파이: `sudo apt install alsa-utils`
- **B (libasound)**  
  - 개발 패키지: `sudo apt install libasound2-dev`  
  - 링크: `-lasound`

---

### Step 6: (선택) 동시 수신과 재생

- 한 클라이언트 재생 중에 **다른 클라이언트가 연결**되면:
  - **대기**: 재생이 끝난 뒤 `accept()` 처리 (현재처럼 단일 스레드면 자연스럽게 대기).
  - **재생만 별도 스레드**: 수신 스레드는 받은 데이터를 **재생 큐**에 넣고 바로 다음 `accept()` 로 넘어가고, **재생 전용 스레드**가 큐에서 꺼내 재생.  
    → 동시 접속은 받되, 재생은 순차 처리하는 방식으로 정리 가능.

---

## 구현 체크리스트 요약

| 단계 | 내용 | 상태 |
|------|------|------|
| 1 | 오디오 포맷 상수 정의 (샘플레이트, 채널, S16_LE) 및 클라이언트-서버 규격 문서화 | ✅ 완료 (클라이언트: 16000Hz, 모노, Int16) |
| 2 | 재생 방식 선택: `aplay` 로 시작 후 필요 시 libasound 로 전환 | ✅ 완료 (`popen` + `aplay` 사용) |
| 3 | 수신 완료 후 저장된 파일(또는 메모리 버퍼)로 재생 호출 (우선 3-1) | ✅ 완료 (메모리 버퍼 사용, 파일 저장 없음) |
| 4 | 재생 디바이스 `default` (오디오 잭) 확인 및 사용 | ✅ 완료 (`-D default` 사용) |
| 5 | alsa-utils 설치 (aplay) 또는 libasound2-dev (ALSA API) | ⚠️ 런타임 의존성 (aplay 필요) |
| 6 | (선택) 재생을 별도 스레드/큐로 분리해 동시 수신 처리 | ⏸️ 미구현 (현재는 재생 완료 후 다음 accept) |

---

## 구현 내용 (완료)

### 구현 방식

- **방식**: 메모리 버퍼에서 바로 재생 (파일 저장 없음)
- **재생 방법**: `popen("aplay ...", "w")` + `fwrite()` 파이프 전달
- **포맷**: 16000 Hz, 모노(1ch), S16_LE

### 코드 위치

`server/src/main.cpp` 의 `run_audio_receiver()` 함수

### 구현 세부사항

1. **메모리 버퍼 수집**
   ```cpp
   std::vector<char> audio_buffer;
   while ((bytes = read(client_fd, buf, sizeof(buf))) > 0) {
       audio_buffer.insert(audio_buffer.end(), buf, buf + bytes);
   }
   ```

2. **ALSA 재생**
   ```cpp
   FILE* aplay = popen("aplay -f S16_LE -r 16000 -c 1 -D default", "w");
   if (aplay) {
       size_t written = fwrite(audio_buffer.data(), 1, audio_buffer.size(), aplay);
       pclose(aplay);
   }
   ```

3. **에러 처리**
   - 빈 데이터 체크
   - 재생 실패 시 에러 메시지 출력

### 동작 흐름

1. 클라이언트가 TCP(포트 5556)로 음성 데이터 전송
2. 서버가 메모리 버퍼(`std::vector<char>`)에 수신 데이터 수집
3. 수신 완료 후 `popen`으로 `aplay` 프로세스 실행
4. 버퍼 데이터를 파이프로 `aplay`에 전달하여 오디오 잭으로 재생
5. 재생 완료 후 다음 클라이언트 연결 대기

### 의존성

- **런타임**: `aplay` 명령어 필요 (`alsa-utils` 패키지)
- **빌드**: `#include <cstdio>` (popen/pclose용)

---

## 참고: aplay 한 줄 예시

```bash
# 저장된 RAW 파일 재생 (16kHz, 모노, 16비트)
aplay -f S16_LE -r 16000 -c 1 -D default voice_recs/voice_20250101_120000.raw
```

현재 구현에서는 파일 저장 없이 메모리 버퍼를 파이프로 직접 전달합니다.

---

## 다음 구현 계획: 링 버퍼 + Playback Thread (libasound)

`aplay + popen` 방식은 **프로세스 생성/종료 오버헤드**와 **디폴트 버퍼링** 때문에 “버튼 누르면 바로 들리는” UX에서 지연이 체감될 수 있습니다.  
다음 단계로 **libasound(=ALSA API)** 를 사용해 **수신과 재생을 분리**하고, 재생 버퍼를 직접 튜닝하는 구조로 개선합니다.

### 목표

- 수신된 오디오를 **받는 즉시(스트리밍)** 오디오 잭으로 재생
- `snd_pcm_*` 설정으로 **latency(버퍼/period)** 를 줄여 체감 지연 감소

### 전체 구조

- **Receiver Thread (Producer)**: TCP로 받은 RAW PCM(S16_LE, 16kHz, mono)을 **링 버퍼**에 push
- **Playback Thread (Consumer)**: 링 버퍼에서 frame을 꺼내 **`snd_pcm_writei()`** 로 계속 출력

### Step A: 공용 오디오 설정 상수화

- `AUDIO_SAMPLE_RATE = 16000`
- `AUDIO_CHANNELS = 1`
- `AUDIO_FORMAT = SND_PCM_FORMAT_S16_LE`
- `FRAME_BYTES = 2 * AUDIO_CHANNELS` (Int16 1샘플 = 2바이트)

### Step B: 링 버퍼(Producer/Consumer) 설계

- **단위**: “바이트”가 아니라 **프레임 단위**(frame = sample * channels)로 다루는 것을 권장
- **동기화**:
  - 간단히: `std::mutex + std::condition_variable` 로 보호
  - 성능 우선: lock-free ring buffer(추후)
- **정책**:
  - 오디오 잭이 느리거나 순간적으로 burst가 크면 버퍼가 찰 수 있음 → **drop(최신 유지/최초 유지)** 중 하나를 명시

### Step C: ALSA(PCM) 초기화 (Playback Thread 시작 시 1회)

1. `snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0)`
2. `snd_pcm_set_params(...)` 또는 `snd_pcm_hw_params_*` 로 포맷/레이트/채널 설정
3. **latency 튜닝**:
   - period/buffer를 작게(예: period 10~20ms, buffer 40~80ms 수준부터 시작)
4. underrun(XRUN) 대응:
   - `snd_pcm_writei` 가 `-EPIPE` 반환 시 `snd_pcm_prepare(pcm)` 후 재시도

### Step D: Playback Thread 루프 (Consumer)

- 목표: 항상 “period frames” 만큼 확보해서 `snd_pcm_writei()` 호출
- 흐름:
  1. 링 버퍼에서 **필요 프레임만큼** 꺼내 local buffer에 채움
  2. 부족하면:
     - 잠깐 대기(조건변수) 또는
     - “무음(0)” padding으로 underrun 방지 (UX 선택)
  3. `snd_pcm_writei(pcm, frames, frame_count)` 호출

### Step E: Receiver Thread 수정 (Producer)

- 현재처럼 `accept()` 후 `read()`로 받은 `buf`를:
  - 링 버퍼에 push (프레임 정렬: `bytes % FRAME_BYTES == 0` 확인/보정)
- “버튼 누르면 바로 들리게”를 목표로 하면:
  - **수신 즉시 push**(파일/메모리 전체 누적 금지)
  - 서버는 클라이언트가 연결된 동안 스트리밍, 끊기면 playback은 drain 후 idle

### Step F: 종료/상태 전파

- SIGINT 등 종료 시:
  - receiver thread 종료
  - playback thread에 종료 플래그 + condition notify
  - `snd_pcm_drain()` 또는 `snd_pcm_drop()` 후 `snd_pcm_close()`

### Step G: 빌드 설정

- 패키지: `sudo apt install libasound2-dev`
- 링크: **`-lasound`**
- CMake를 쓰는 경우 `target_link_libraries(<server> asound)` 추가

### 구현 산출물(권장 파일 분리)

- `server/include/audio_playback.h`
- `server/src/audio_playback.cpp`  (ALSA init + playback thread + ring buffer)
- `server/src/main.cpp` 는 “수신 → 링버퍼 push” 와 “playback start/stop”만 호출

### 커스텀 오디오 디바이스 드라이버(커널) 구현은?

현재 요구사항(오디오 잭 출력, PCM 재생, latency 튜닝) 기준으로는 **커널 드라이버를 새로 만드는 건 난이도/시간 대비 이득이 거의 없고 유지보수 부담이 큽니다.**  
대부분의 경우 **ALSA(libasound) 레벨**에서 충분히 해결 가능합니다.
