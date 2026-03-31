# SFEPS 프로젝트 PPT 기술 문서

> 구현 담당자 기여도 중심으로 정리한 슬라이드 초안입니다.

---

## SLIDE 1 — 프로젝트 개요

**SFEPS** (Smart Fare Evasion Prevention System)  
지하철 부정승차를 실시간으로 감지·추적·신고하는 통합 임베디드 관제 시스템

### 전체 시나리오 흐름
```
① 승객 진입
      │
② RFID 태그 (RC522 커널 드라이버 → UDS → 서버)
      │
③ ONVIF 카메라 분석 (메타데이터 bbox → 딥러닝 트래커)
      │
④ 데이터 불일치 판별 (카드 등급 vs 카메라 분류)
      │
⑤ 가상선 침범 감지 (바운딩박스 위치 기반)
      │
⑥ 부정승차 확정 → 관리자 알림 (Qt 관제 클라이언트)
      │
⑦ 레이저 객체 트래킹 (STM32 서보 PWM 제어)
      │
⑧ 관리자 개입 (오디오 방송 / 알림)
```

---

## SLIDE 2 — 전체 시스템 구조 (HW/SW 아키텍처)

### 하드웨어 구성

```
┌─────────────────────────────────────────────────────────────────────┐
│  라즈베리파이 (서버 노드)                                            │
│                                                                      │
│  ┌──────────────┐    SPI    ┌────────────────┐                       │
│  │  RC522 RFID  │◄─────────│  커널 드라이버  │ /dev/rc522            │
│  │   모듈       │           │  (rc522_spi.c) │                       │
│  └──────────────┘           └───────┬────────┘                       │
│                                     │ ioctl (RC522_READ_CARD)         │
│                             ┌───────▼────────┐                       │
│                             │ UDS 데몬        │ /tmp/rc522_events.sock│
│                             │(rc522_uds_      │ NDJSON 스트림         │
│                             │ daemon.cpp)    │                        │
│                             └───────┬────────┘                       │
│                                     │ Unix Domain Socket              │
│  ┌──────────────────────────────────▼──────────────────────────────┐ │
│  │  C++ 서버 프로세스(server/)                                     │ │
│  │                                                                  │ │
│  │  rfid_monitor ──► analytics ──► alert_service                   │ │
│  │  audio_service (TCP 5556, libalsa 재생)                          │ │
│  │  position_service (TCP 5565, 레이저 좌표 수신)                   │ │
│  │  tls_server (HTTPS/WSS)                                          │ │
│  └──────────────────────────────────────────────────────────────────┘ │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────────┐ │
│  │  오디오 스피커 유닛 (Audio_Speaker_Unit/)                        │ │
│  │  TCP 5556 수신 → AudioRingBuffer → libalsa (snd_pcm_writei)     │ │
│  └──────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│  Windows PC (Qt 관제 + 카메라 분석 통합 노드)                         │
│                                                                      │
│  Client/src/videobackend.cpp + camera_RBF(Qt mode)                  │
│  ├── ONVIF 메타데이터 수신 (RTSPClient/XMLParser)                    │
│  ├── rbfqt_process_metadata(...) 로 tracker/ID 갱신                  │
│  ├── rbfqt_compute_pwm(...) 33ms 주기 호출                           │
│  ├── DeepSORT/IdStabilizer/KalmanBbox2D (camera_RBF 내부 로직)      │
│  ├── QtPoseWorker + MediaPipe (어깨 랜드마크 기반 aim 계산)          │
│  ├── VoiceManager (QAudioSource → TCP 5556 스트리밍)                 │
│  ├── FraudManager (부정승차 알림 수신)                               │
│  ├── PositionManager (레이저 좌표 TCP 5565 송신)                      │
│  └── MainWindow (Qt GUI)                                             │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│  STM32 Nucleo-F401RE (임베디드 레이저 트래킹)                       │
│                                                                      │
│  ESP-8266 ──UART──► STM32                                            │
│  (WiFi TCP 5555)   (USART1)                                          │
│                       │                                              │
│               ┌───────┴───────┐                                     │
│               │  TIM1 (PA8)   │  서보 Y축 PWM                       │
│               │  TIM2 (PA0)   │  서보 X축 PWM                       │
│               │  PA5  (LED)   │  레이저 ON/OFF 테스트 공용          │
│               └───────────────┘                                     │
│               Prescaler=83 → 1tick=1µs → 50Hz(20ms) 서보 표준      │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│  ONVIF IP 카메라                                                      │
│  RTSP 스트림 + 메타데이터(ONVIF WS Event)                            │
│  → Windows Qt Client(videobackend + camera_RBF Qt mode)로 전달      │
└─────────────────────────────────────────────────────────────────────┘
```

### SW 레이어 다이어그램
```
[Windows Qt Client(통합)]                  [라즈베리파이 서버]
        │                                          │
  videobackend.cpp                                 rfid_monitor
  + camera_RBF(Qt mode)                            audio_service
  (rbfqt_process_metadata / compute_pwm)           alert_service
        │                                          │
  VoiceManager ───────── TCP 5556 ────────────────► audio_service
  PositionManager ────── TCP 5565 ────────────────► position_service
        │
[STM32 ESP-8266]
  [서보 PWM 제어]
```

---

## SLIDE 3 — ① RFID 디바이스 드라이버

### 왜 커널 드라이버로 구현했는가?
- **RC522 (MFRC522)** 는 SPI 통신 기반의 RFID 리더 칩
- User-space에서 spidev로 직접 제어 시: polling 오버헤드, 인터럽트 처리 어려움
- 커널 드라이버로 구현하면 `/dev/rc522` 캐릭터 디바이스 노출 → 표준 POSIX API(`read`, `ioctl`)로 사용 가능

### 구현 3계층 구조

```
┌──────────────────────────────────────────────────────────┐
│  Layer 3: UDS 데몬 (rc522_uds_daemon.cpp)                │
│  - /dev/rc522 열기 → ioctl(RC522_READ_CARD) 반복         │
│  - 태그 감지 시 text sector도 읽어 NDJSON 포맷 구성      │
│  - Unix Domain Socket /tmp/rc522_events.sock 으로 publish│
│  - 데몬화 (fork/setsid) 지원                             │
├──────────────────────────────────────────────────────────┤
│  Layer 2: 캐릭터 디바이스 (rc522_chardev.c)              │
│  - /dev/rc522 misc device 등록                           │
│  - read()  → rc522_read_uid_blocking() → UID 4바이트    │
│  - ioctl() → RC522_READ_CARD, RC522_READ_TEXT_SECTOR     │
│             RC522_WRITE_REG (CLASS 기록용)               │
├──────────────────────────────────────────────────────────┤
│  Layer 1: 코어 로직 (rc522_core.c)                       │
│  - rc522_request / rc522_anticoll / rc522_select_tag     │
│  - rc522_authenticate (MIFARE Crypto1 인증)              │
│  - rc522_read_block / rc522_write_block (16바이트 단위)  │
│  - rc522_read_uid_blocking / rc522_read_text_sector_     │
│    blocking / rc522_write_text_sector_blocking           │
└──────────────────────────────────────────────────────────┘
           │ SPI 버스
┌──────────▼──────────────────────────────────────────────┐
│  RC522 칩 (MFRC522)                                      │
│  - REQA → Anticollision → Select → Authenticate → R/W   │
└──────────────────────────────────────────────────────────┘
```

### READ / WRITE 구분

| 동작 | 함수 | 사용 목적 |
|------|------|----------|
| **READ** (운영) | `rc522_read_uid_blocking()` + `rc522_read_text_sector_blocking()` | 카드 UID + 저장된 CLASS 텍스트 읽기 → 부정승차 판별 |
| **WRITE** (초기화) | `rc522_write_text_sector_blocking()` | 카드별 클래스 지정 (성인/청소년/어린이 등) 기록 |

### RFID 이벤트 전달 흐름

```
RC522 칩
  │ SPI
rc522_core.c
  │ rc522_to_card() — TRANSCEIVE/AUTHENT
  │ rc522_read_uid_blocking() : REQIDL → Anticoll → Select → return UID
  │
rc522_chardev.c                    rc522_ioctl.h
  │ ioctl(RC522_READ_CARD, &uid)
  │ ioctl(RC522_READ_TEXT_SECTOR, &text_data)  ← CLASS 정보
  │
rc522_uds_daemon.cpp
  │ format: {"device_id":1,"id":"AABBCCDD","text":"adult","timestamp":1234}
  │ Unix Domain Socket → /tmp/rc522_events.sock
  │
server/rfid_monitor.cpp
  │ connect() → poll() → read() → JSON 파싱
  │ extract_json_value(json, "text") → card_age_text
  │
analytics.cpp
  └─ onRfidRead(card_age_text)  →  부정승차 판별 로직
```

### 핵심 코드 포인트

```c
// ▶ 커널 드라이버 READ — Blocking 방식
int rc522_read_uid_blocking(struct rc522_dev *dev, u32 *out_uid)
{
    while (1) {
        if (signal_pending(current)) return -ERESTARTSYS;
        if (rc522_read_uid_no_block(dev, out_uid) == 0) return 0;
        dev->ops->msleep(100);  // 100ms polling 간격
    }
}

// ▶ UDS 데몬 — ioctl로 카드 읽기 → JSON 전송
while (g_running) {
    ioctl(rc522_fd, RC522_READ_CARD, &uid);          // UID 읽기
    ioctl(rc522_fd, RC522_READ_TEXT_SECTOR, &text_data); // CLASS 읽기
    std::string line = format_tag_event(uid, text, ts);  // NDJSON 생성
    write(client_fd, line.data(), line.size());           // UDS 전송
}

// ▶ 서버 — poll 기반 이벤트 수신
while (m_running) {
    poll(&pfd, 1, 1000);   // 1초 타임아웃
    read(sock_fd, buffer, sizeof(buffer));
    // {"id":"AABBCCDD","text":"adult","timestamp":...} 파싱
    process_rfid_tag(uid, card_age_text, time_str);
}
```

---

## SLIDE 4 — ③ 실시간 관제 오디오 통신 (libalsa)

### 왜 libalsa(ALSA)를 직접 사용했는가?
- Qt 또는 PulseAudio 레이어를 건너뜀 → **지연(latency) 최소화**
- `snd_pcm_set_params()` 에서 **샘플링 주파수·버퍼 크기를 직접 제어**
  - 16kHz 모노: 음성 품질 충분, 데이터량 최소 (32KB/s)
  - period 50ms: XRUN(underrun) 방지와 실시간 재생 사이 균형
- `AudioRingBuffer`: 네트워크 지터 흡수용 약 1초 완충 버퍼
- 재생 스레드 별도 분리 → TCP 수신과 재생이 독립적으로 동작 (스트리밍)

### 전체 오디오 파이프라인

```
[Qt 클라이언트 (Windows)]                [라즈베리파이 서버]
                                                                   
QAudioSource                          server/audio_service.cpp
  ├── 포맷: 16kHz, mono, S16_LE       ├── TCP 5556 (일반) / 6556 (TLS)
  └── SocketForwardDevice             ├── poll() → accept() → read()
       │                              │    chunk 단위(4096B)로 수신
       │ TCP 5556 스트리밍            │
       └──────────────────────────────►  AudioRingBuffer::push(buf, bytes)
                                                   │
                                      별도 재생 스레드 (libalsa)
                                      AudioPlayback::playbackThreadFunc()
                                           │
                                      ring.pop(period_frames)
                                           │
                                      snd_pcm_writei(pcm_handle_, ...)
                                           │
                                       [스피커 / 헤드셋 출력]
                                       
RFID 태그 이벤트 발생 시:
  make_rfid_tag_tone() → 2400Hz, 120ms 비프음 생성
  ring->push(tone_pcm, ...) → 동일 링버퍼로 바로 주입 → 스피커 출력
```

### AudioRingBuffer 구조

```
┌─────────────────────────────────────────────────────────────┐
│  AudioRingBuffer (circular buffer, ~32KB = 1초 분량)         │
│                                                              │
│  Producer (TCP 수신 스레드)      Consumer (ALSA 재생 스레드) │
│  push(data, bytes)               pop(out, period_bytes)      │
│       │                                   │                  │
│       │ mutex + condition_variable         │                  │
│       │ 버퍼 full → cv_not_full_.wait()   │                  │
│       │ 버퍼 empty → cv_not_empty_.wait() │                  │
│       ▼                                   ▼                  │
│  [head] ←←←←←← data flow ←←←←←← [tail]                   │
│                                                              │
│  용량: AUDIO_SAMPLE_RATE * AUDIO_FRAME_BYTES * 1            │
│       = 16000 * 2 * 1 = 32,000 bytes ≈ 1초                  │
└─────────────────────────────────────────────────────────────┘
```

### ALSA 초기화 핵심 파라미터

```c
// audio_playback.cpp — initPcm()
snd_pcm_open(&pcm_handle_, "default", SND_PCM_STREAM_PLAYBACK, 0);

snd_pcm_set_params(
    pcm_handle_,
    SND_PCM_FORMAT_S16_LE,        // 16bit signed little-endian
    SND_PCM_ACCESS_RW_INTERLEAVED,// interleaved read/write
    AUDIO_CHANNELS,               // 1 (mono)
    AUDIO_SAMPLE_RATE,            // 16000 Hz
    1,                            // soft_resample 허용
    50000                         // latency 50ms (튜닝 가능)
);

// XRUN(underrun) 처리
if (snd_pcm_writei(...) == -EPIPE) {
    snd_pcm_prepare(pcm_handle_);  // 복구 후 재시도
}
```

### 오디오 포맷 통일 (클라이언트 ↔ 서버 동일)

| 파라미터 | 값 | 이유 |
|---------|-----|------|
| Sample Rate | 16,000 Hz | 음성 충분, 데이터량 절반 |
| Channels | 1 (mono) | 방송용 단방향 |
| Format | S16_LE (Int16) | ALSA 기본 포맷, Qt QAudioFormat 동일 |
| Frame Bytes | 2 bytes | 1 sample × 1 channel |

---

## SLIDE 5 — ④ 레이저 객체 트래킹 (임베디드 핵심)

### 기술 선택 과정 — 왜 이 방법을 쓰게 됐는가?

```
시도 1: 모노 카메라 캘리브레이션
  │  문제: 오차 약 10% 발생
  │        → 실제 3D 좌표(X,Y,Z)를 정확히 얻을 수 없음
  ▼
시도 2: Z-plane Homography (체커보드를 바닥에 배치)
  │  문제: 강의장 환경 특성 상 책상·모니터가 bbox를 가림
  │        → 발 위치(바닥 평면) 정확한 추정 불가
  ▼
시도 3: Z=1200mm 평면 상 점 16개 직접 측정 → RBF(TPS) 보간
         ★ 채택
  │  과정: 바닥에서 1200mm 높이 평면에 실제 위치한 점의
  │        픽셀 좌표(u,v) ↔ 서보 PWM(pan,tilt) 직접 측정
  │  문제: 사람의 "1200mm 지점"이 어디인지 특정이 어려움
  ▼
해결: RFID 태그 시 MediaPipe Pose로 어깨-골반 비율 측정
       → 개인별 ratio 저장 → 이후 추적에 활용
       target_v = bbox_top + bbox_height × ratio
```

### 전체 레이저 트래킹 파이프라인

```
ONVIF 카메라
    │ RTSP 스트림 + 메타데이터 XML
    ▼
Windows Qt Client (Client/src/videobackend.cpp)
    │
    ├── ONVIF bbox 파싱 (RTSPClient + XMLParser)
    │      → applyNativeDetections(...)
    │      → rbfqt_process_metadata(humans, W, H)
    │
    ├── camera_RBF Qt mode 내부 로직
    │      DeepSORT + IdStabilizer + KalmanBbox2D
    │
    ├── QtPoseWorker (videobackend 내 비동기 워커)
    │      mediapipe_pose_worker.py 호출
    │      어깨 landmark(11,12) 기반 aim 계산
    │      target_v = shoulder_y + (bbox_bottom - shoulder_y) × ratio
    │
    ├── rbfqt_compute_pwm(now,W,H,&pan,&tilt) (33ms tick)
    │      RBF TPS 보간 + 예측 + 스무딩
    │
    └── emit pwmSetRequested(pan, tilt)
                 │
                 ├── TCP 5565: server position_service
                 └── TCP 5555: ESP8266 → STM32 USART1
                                                              │
                                                    servo_driver.c
                                                    TIM1(PA8) Y축
                                                    TIM2(PA0) X축
                                                    PA5 = 레이저 ON/OFF
```

### KalmanBbox2D — 왜 필요한가?

```
카메라 메타데이터 딜레이: ~150~300ms 존재
  → 현재 프레임의 bbox는 이미 "과거" 위치

KalmanBbox2D 동작:
  상태: [cx, cy, vx, vy, w, h]
  
  update(측정값, dt):
    예측: px = cx + vx*dt, py = cy + vy*dt
    잔차: rx = meas_cx - px
    갱신: cx = px + α*rx        (α=0.6: 새 측정값 반응도)
          vx += (β*rx) / dt     (β=0.15: 속도 학습률)
    outlier gate: |rx|>120px → 속도 초기화 (튐 방지)
  
  predict(dt_ahead=300ms):
    pred_cx = cx + vx * 0.3     ← 미래 위치 예측
    pred_cy = cy + vy * 0.3

결과: 레이저가 현재 위치가 아닌 "도착 시점"의 위치를 미리 겨냥
```

### RBF Thin-Plate Spline — 왜 쓰는가?

```
문제: 렌즈 왜곡 + 투영 비선형성으로 단순 선형 보간 불가

RBF TPS (Thin-Plate Spline):
  피팅: N개 캘리브레이션 포인트 (pixel u,v) → PWM값
  
  캘리브레이션 포인트 예시 (16개):
  (-370cm, 65cm) → pixel(80, 405)   → PWM(890, 1320)
  (-370cm, 201cm)→ pixel(401, 285)  → PWM(1055,1390)
  ...

  TPS 커널: φ(r) = r² log(r)   (r = 유클리드 거리)
  해: [w1..wN, a, b, c] = A^(-1) * Y  (OpenCV SVD 풀이)
  
  eval(u,v):
    result = Σ w_i * φ(‖(u,v)-(u_i,v_i)‖) + a*u + b*v + c
```

### pose_ratio_calib.py — 1200mm 지점 비율 측정

```python
# RFID 태그 시 1회 캘리브레이션 실행
# MediaPipe Pose → 사람 bbox(top/bottom) 감지
# 마우스 클릭으로 "이 픽셀이 1200mm 지점"이라고 지정
# ratio = (click_y - bbox_top) / bbox_height
# → JSON 저장

# 이후 추적 시:
target_v = bbox_top + bbox_height * ratio  # 개인별 1200mm 지점
# (u, target_v) 를 RBF에 입력 → PWM 계산
```

### 캘리브레이션 실패 단계 정리 (PPT 도식화용)

```
┌─────────────────────────────────────────────────────────┐
│  캘리브레이션 진화 과정                                   │
│                                                          │
│  Step 1: 모노 카메라 캘리브레이션                        │
│  ├── 체커보드 → intrinsics 추출                          │
│  └── ❌ 오차 10% → 실제 3D 거리 측정 불가               │
│                                                          │
│  Step 2: Z-plane Homography                              │
│  ├── 바닥 체커보드 → 2D→2D 변환 행렬                   │
│  └── ❌ 가구/책상으로 발 위치 가림 → 정확도 저하         │
│                                                          │
│  Step 3: Z=1200mm RBF (채택)                             │
│  ├── 16개 실측 포인트: (X_cm, Y_cm) ↔ (u,v) ↔ PWM     │
│  ├── TPS 보간 → 임의 픽셀 → PWM 직접 변환               │
│  ├── ⚠️ 문제: 사람의 1200mm 지점 특정 어려움             │
│  └── ✅ 해결: MediaPipe로 어깨-골반 비율 → ratio 저장   │
└─────────────────────────────────────────────────────────┘
```

---

## SLIDE 6 — 가상 시나리오 기술 매핑

```
시나리오 단계            적용 기술                    구현 위치
────────────────────────────────────────────────────────────────────
① 승객 진입
                         
② RFID 태그          ← rc522_core.c (커널 드라이버)  hardware/Raspi-driver
                       rc522_chardev.c (캐릭터 디바이스) Kernel_Driver/
                       rc522_uds_daemon.cpp (UDS 데몬)  Server_examples/
                       rfid_monitor.cpp (서버 수신)      server/src/modules/
                       
③ 카메라 분석        ← videobackend.cpp + rbfqt        Client/src/
                       camera_RBF.cpp(Qt mode 라이브러리) Camera/get_metadata/
                       XMLParser.cpp (메타데이터 파싱)
                       DeepSORT (ID 안정화)
                       IdStabilizer (Hungarian 매핑)
                       
④ 데이터 불일치      ← analytics.cpp                   server/src/modules/
                       card_age_text vs bbox 위치 비교
                       
⑤ 가상선 침범        ← position_service.cpp            server/src/modules/
                       바운딩박스 y좌표 임계값 비교
                       
⑥ 부정승차 확정      ← alert_service.cpp               server/src/modules/
                       FraudManager.cpp (Qt 클라이언트) client_msvc/
                       
⑦ 레이저 추적        ← KalmanBbox2D (딜레이 보상)      Camera/get_metadata/
                       RBF TPS 보간 (픽셀→PWM)
                       MediaPipe Pose (어깨 비율)
                       STM32 servo_driver.c (PWM 출력)  hardware/stm32-laser/
                       ESP-8266 TCP 브릿지
                       
⑧ 관리자 개입        ← audio_service.cpp (서버 수신)   server/src/modules/
                       AudioRingBuffer + libalsa 재생   hardware/Raspi-driver/
                       VoiceManager.cpp (Qt 마이크)     client_msvc/
                       QAudioSource → TCP 5556 스트리밍
```

---

## SLIDE 7 — 핵심 기여 기능 요약표

| 기능 | 핵심 기술 | 라이브러리/프레임워크 | 파일 위치 |
|------|-----------|----------------------|-----------|
| RFID 커널 드라이버 | SPI, MIFARE Crypto1, Linux misc device | Linux Kernel API | `Kernel_Driver/` |
| RFID UDS 데몬 | POSIX socket, ioctl, daemonize | POSIX C++ | `Server_examples/rc522_uds_daemon.cpp` |
| 실시간 오디오 재생 | Ring Buffer, ALSA PCM | libalsa (libasound) | `Audio_Speaker_Unit/` |
| 오디오 스트리밍 클라이언트 | RAW PCM TCP 스트리밍 | Qt (QAudioSource, QTcpSocket) | `client_msvc/voicemanager.cpp` |
| 레이저 RBF 캘리브레이션 | Thin-Plate Spline 보간 | OpenCV (SVD) | `camera_RBF.cpp` |
| 카메라 딜레이 보상 | 속도 기반 칼만 필터 | OpenCV | `camera_RBF.cpp::KalmanBbox2D` |
| 어깨 비율 캘리브레이션 | MediaPipe Pose (landmark 11,12) | MediaPipe, OpenCV | `pose_ratio_calib.py` |
| 서보 PWM 제어 | TIM1/TIM2, 1tick=1µs, 50Hz | STM32 HAL | `stm32/Core/Src/servo_driver.c` |
| ESP-8266 TCP 브릿지 | AT 명령, DMA+IDLE 수신 | STM32 HAL, USART1 | `stm32/Core/Src/main.c` |

---

## SLIDE 8 — 한계점 및 개선 방향

### 현재 한계
1. **RFID 100ms polling**: 비접촉 태그 감지 간격으로 빠른 태깅 시 누락 가능
   - 개선: IRQ 기반 드라이버로 교체 (`Kernel_Driver_irq/` 버전 구현됨)

2. **RBF 캘리브 포인트 한계**: 16개 측정점 → 외삽 영역 정확도 저하
   - 개선: 포인트 수 증가 또는 뉴럴 보간으로 교체

3. **MediaPipe 비동기 지연**: 3프레임마다 추론 → 빠른 움직임 시 어깨 위치 stale
   - 개선: 경량 모델(MoveNet Lightning) 또는 GPU 가속

4. **1200mm ratio 고정**: 카드 태그 시 측정한 ratio를 이후에도 동일하게 사용
   - 개선: 실시간 MediaPipe로 매 프레임 갱신

5. **오디오 단방향**: 서버(스피커)→클라이언트(마이크) 단방향 스트리밍
   - 개선: 양방향 duplex (echo cancellation 필요)

### 확장 가능성
- RFID 데이터 → 클라우드 DB 연동 → 빅데이터 부정승차 패턴 분석
- STM32 레이저 → 더 정밀한 2-DOF 짐벌 + 고출력 모듈로 교체
- RBF → SLAM 기반 동적 캘리브레이션 (환경 변화 대응)
- 오디오 → TTS 엔진 연동 자동 안내방송
