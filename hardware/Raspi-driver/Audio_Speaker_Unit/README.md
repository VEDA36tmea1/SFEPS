# Audio_Speaker_Unit – RAW/MP3 오디오 스피커 유닛

라즈베리 파이에서 **네트워크로 받은 음성을 바로 스피커/헤드셋으로 재생**하기 위한 실험용 유닛입니다.  
최종 목표는 여기서 검증한 모듈을 `server/` 쪽으로 옮겨, 매니저가 Qt 클라이언트에서 마이크 버튼을 누르면 **즉시 라즈베리 파이 스피커에서 출력**되도록 하는 것입니다.

---

## 1. 파일 구조 및 역할

| 파일 | 역할 |
|------|------|
| `audio_common.h` | 오디오 포맷 공통 상수 (`AUDIO_SAMPLE_RATE=16000`, `AUDIO_CHANNELS=1`, `AUDIO_FRAME_BYTES=2` 등) |
| `audio_ring_buffer.h/.cpp` | 멀티스레드용 오디오 링 버퍼 (`AudioRingBuffer`) – `push()`/`pop()`/`stop()` |
| `audio_playback.h/.cpp` | ALSA(libasound)를 이용한 재생 스레드 (`AudioPlayback`) – `snd_pcm_writei()` 로 계속 출력 |
| `main.cpp` | 테스트용 실행 파일 진입점 – TCP로 RAW/MP3 수신 후 스피커로 재생 |
| `IMPLEMENTATION_PLAN.md` | 전체 설계 및 구현 계획 문서 |
| `Dev.md` | 현재까지 구현한 내용과 다음 단계 개발 노트 |

---

## 2. RAW 모드 구조 (기본 동작, 스트리밍)

### 2-1. 전체 흐름

- **수신 스레드(Receiver)**: `run_tcp_receiver_raw()`  
  - `AUDIO_TEST_PORT`(기본 6000) 에서 TCP listen  
  - 클라이언트가 연결되면 `read()` 로 들어오는 RAW PCM(16kHz, mono, S16_LE)을 **곧바로 `AudioRingBuffer::push()`** 로 밀어넣음
  - 링 버퍼가 가득 차면, 잠깐 block 되어 공간이 생길 때까지 기다림

- **재생 스레드(Playback)**: `AudioPlayback::playbackThreadFunc()`  
  - 시작 시 `snd_pcm_open("default")` + `snd_pcm_set_params(...)` 로 ALSA PCM 초기화  
  - 링 버퍼에 데이터가 들어오기 시작하면, **period 단위로 `ring.pop()` → `snd_pcm_writei()`** 를 반복  
  - XRUN(언더런) 발생 시 `snd_pcm_prepare()` 로 복구 후 재시도

즉, **“수신 → 링 버퍼 → 재생”이 동시에 진행되는 스트리밍 구조**이고,  
`aplay + popen` 처럼 “전체를 다 받은 뒤 한 번에 재생”하는 방식이 아닙니다.  
링 버퍼 용량(대략 1초 분량)은 **네트워크/재생 타이밍 차이를 완충하는 버퍼** 역할만 합니다.

---

## 3. MP3 모드 구조 (옵션)

RAW PCM 대신 **MP3 파일을 통째로 보내고 바로 재생**하는 간단한 테스트 경로입니다.

- `./audio_speaker_test --mp3` 로 실행하면 **MP3 모드**로 동작합니다.
- 포맷: MP3 (압축) – PCM 디코딩/재생은 외부 플레이어(`mpg123`)가 담당.

### 3-1. 흐름

- **Receiver (MP3)**: `run_tcp_receiver_mp3()`  
  - 포트 6000 에서 TCP listen  
  - 클라이언트가 MP3 바이트 스트림을 보내면,  
    `popen("mpg123 -q -", "w")` 를 통해 `mpg123` 에 그대로 파이프로 전달  
  - `mpg123` 가 MP3 → PCM 디코드 후 ALSA 디바이스로 출력

- 링 버퍼/`AudioPlayback` 은 사용하지 않고,  
  **“네트워크 → mpg123 → ALSA” 직결 경로**를 쓰는 모드입니다.

> MP3 모드는 “압축 파일 보내서 바로 재생”을 빠르게 확인하기 위한 모드이고,  
> 최종 구조(서버 통합)는 RAW + libasound 파이프라인을 기준으로 설계하는 것이 좋습니다.

---

## 4. 빌드 및 실행 방법

### 4-1. 라즈베리 파이에서 빌드

```bash
cd ~/SFEPS/hardware/Raspi-driver/Audio_Speaker_Unit
make
```

- 생성 파일: `audio_speaker_test`
- 필요 패키지:
  - RAW 모드만: `libasound2-dev` (빌드용), ALSA 기본 드라이버
  - MP3 모드: `mpg123` (런타임, `sudo apt install mpg123`)

### 4-2. RAW 모드 테스트 (PCM 스트리밍)

1. **라즈베리 파이에서 실행**:

    ```bash
    cd ~/SFEPS/hardware/Raspi-driver/Audio_Speaker_Unit
    ./audio_speaker_test
    # → RAW 모드, 포트 6000에서 대기
    ```

2. **PC 또는 다른 머신에서 RAW 파일 전송** (`tmp_client/send_audio` 사용 예):

    ```bash
    cd ~/SFEPS/tmp_client
    make               # send_audio 빌드 (1회만)

    # 예: 파이 IP가 192.168.0.50, 테스트 파일이 test.raw 일 때
    ./send_audio 192.168.0.50 6000 test.raw
    ```

- 포맷: `test.raw` 는 **16kHz, mono, S16_LE** 이어야 합니다.
- 전송이 시작되면, 파이 쪽에서 **바로 링 버퍼에 쌓이고 Playback Thread가 재생**합니다.

### 4-3. MP3 모드 테스트

1. **라즈베리 파이에서 실행**:

    ```bash
    cd ~/SFEPS/hardware/Raspi-driver/Audio_Speaker_Unit
    ./audio_speaker_test --mp3
    # → MP3 모드, 포트 6000에서 대기
    ```

2. **PC 에서 MP3 파일 전송**:

    ```bash
    cd ~/SFEPS/tmp_client
    ./send_audio 192.168.0.50 6000 test.mp3
    ```

- 파이에서는 `mpg123`가 MP3를 디코딩하여 바로 스피커로 출력합니다.

---

## 5. 각 파일 역할 정리 (요약)

| 파일 | 역할 |
|------|------|
| `audio_common.h` | 오디오 포맷 공통 상수 정의 (16kHz, mono, S16_LE) |
| `audio_ring_buffer.h/.cpp` | 멀티스레드 오디오 링 버퍼 (`AudioRingBuffer`) – push/pop/stop |
| `audio_playback.h/.cpp` | ALSA 기반 재생 스레드 (`AudioPlayback`) – `snd_pcm_writei` 루프, XRUN 처리 |
| `main.cpp` | 테스트 실행 진입점: RAW 모드(기본, 링 버퍼 + ALSA), MP3 모드(옵션, mpg123) 선택 및 TCP 수신 |
| `IMPLEMENTATION_PLAN.md` | 설계/구현 계획, 단계별 작업 아이템 |
| `Dev.md` | 실제 구현 진행 내용, 테스트 방법, 향후 TODO 기록 |

---

## 6. 추후 계획 (server 통합)

- 여기서 검증된 **RAW 모드 파이프라인** (링 버퍼 + `AudioPlayback`) 을 `server/` 로 옮겨:
  - `run_audio_receiver()` 에서 TCP로 받은 데이터를 `AudioRingBuffer::push()` 로 넘기고
  - 서버 프로세스 안에서 Playback Thread가 바로 재생
- 이렇게 하면 Qt 클라이언트의 `VoiceManager`(16000Hz, mono, Int16)와  
  라즈베리 파이 서버의 오디오 경로가 깔끔하게 이어지게 됩니다.

