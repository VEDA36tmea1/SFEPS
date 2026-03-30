# Qt 클라이언트 × camera_RBF 통합 구조 문서

> 최종 수정: 2026-03-26  
> 대상 파일: `client_msvc/`, `Camera/get_metadata/src/camera_RBF.cpp`

---

## 1. 개요

| 구성 요소 | 역할 |
|-----------|------|
| `camera_RBF.cpp` | RTSP 스트림 수신 + ONVIF 메타데이터 파싱 + RBF(TPS) 보간으로 Pan/Tilt PWM 계산 |
| `client_msvc` (Qt) | 영상 표시 + Track 버튼으로 추적 대상 선택 + FraudManager 부정승차 감지 |
| `PwmTransmitter` | Qt가 수신한 PWM 값을 Raspberry Pi(TCP) 또는 ESP8266(UDP)으로 전송 |

---

## 2. 전체 데이터 흐름

```
┌─────────────────────────────────────────────────────────────────┐
│ 카메라 (RTSP + ONVIF)                                          │
│   RTSP stream ──→ camera_RBF.cpp                               │
│   ONVIF metadata ──→ camera_RBF.cpp  (bbox 파싱)               │
└─────────────────────────────────────────────────────────────────┘
              │ BCAST_OBJ / OBJ_POS (port 5558)
              ▼
┌─────────────────────────────────────────────────────────────────┐
│ Qt 클라이언트 (client_msvc)                                    │
│                                                                 │
│  PositionManager ──→ 화면에 bbox 오버레이                      │
│  FraudManager   ──→ 부정승차 알림 수신 (port 5557)             │
│                                                                 │
│  [Track 버튼 클릭]                                             │
│    TRACK_START|N1|L=120|T=50|R=300|B=800 (port 5565)          │
│              │                                                  │
│              ▼                                                  │
│  camera_RBF.cpp  ← 선택된 ID bbox 추적 시작                    │
│    → RBF PWM 계산 (Thin-Plate Spline)                         │
│    → PWM_OUT,PAN=1540,TILT=1620 (역방향, 동일 소켓)           │
│              │                                                  │
│              ▼                                                  │
│  PositionManager.pwmReceived(pan, tilt)                        │
│              │                                                  │
│              ▼                                                  │
│  PwmTransmitter.sendPwm(pan, tilt)                             │
└─────────────────────────────────────────────────────────────────┘
              │
     ┌────────┴────────┐
     ▼                 ▼
Raspberry Pi        ESP8266
TCP :5566          UDP :5566
(raspi 모드)       (stm 모드)
```

---

## 3. camera_RBF.cpp — QT 모드 변경사항

### 3.1 `--qt-mode` 플래그

```bash
# 기존 실행 (클릭 즉시 추적)
./camera_RBF rtsp://...

# QT 모드 (Track 버튼 누를 때만 추적 시작)
./camera_RBF rtsp://... --qt-mode
```

| 항목 | 기본 모드 | `--qt-mode` |
|------|-----------|-------------|
| 마우스 클릭 | 클릭한 bbox 즉시 추적 | **무시** |
| 추적 시작 | 클릭 또는 TRACK_START 수신 | **TRACK_START 수신 시만** |
| PWM 출력 | stdout 만 | stdout + **연결 소켓 역방향 전송** |

### 3.2 PWM 역방향 전송 구조

```
remote_select_thread_fn (port 5565)
  → Qt 클라이언트 연결 수신
  → g_remote_client_fd = fd  저장

메인 루프 (프레임마다)
  → sel_ok && frame_id % send_every_n == 0
  → stdout: "SET_PWM,PAN=1540,TILT=1620\n"
  → [--qt-mode] g_remote_client_fd로 "PWM_OUT,PAN=1540,TILT=1620\n" 전송
```

### 3.3 관련 전역 변수 (신규 추가)

```cpp
static bool   g_qt_mode = false;
static SOCKET g_remote_client_fd = INVALID_SOCKET;   // Windows
// static int g_remote_client_fd = -1;               // Linux
static std::mutex g_remote_client_fd_mutex;
```

---

## 4. PositionManager — TRACK_START bbox 자동 포함

### 4.1 동작

`sendPositionCommand("TRACK_START|N1")` 호출 시, 내부 `m_pendingMap`에서 해당 ID의 bbox를 찾아 **자동으로 좌표를 포함**하여 전송합니다.

```
Qt 입력:        TRACK_START|N1
실제 전송:      TRACK_START|N1|L=120|T=50|R=300|B=800
```

### 4.2 목적 — ID 매칭 불일치 보완

Qt의 native tracker와 camera_RBF.cpp의 native tracker는 독립 동작합니다.  
같은 ONVIF 메타데이터를 파싱하지만 타이밍 차이로 `N1`이 서로 다른 사람일 수 있습니다.

camera_RBF.cpp는 ID 매칭 실패 시 전달된 bbox 좌표로 **IoU 매칭을 시도**합니다.

```
camera_RBF 내부 매칭 순서:
  1. g_selected_id == obj.id  → 완전 일치
  2. raw_objs에서 ID 검색     → DeepSORT 안정화 전 원본
  3. has_remote_bbox → IoU 매칭 (bbox 좌표 기반) ← bbox 포함 시 여기서 복구
```

### 4.3 `pwmReceived` 시그널 (신규)

```cpp
// positionmanager.h
signals:
    void pwmReceived(int pan, int tilt);  // camera_RBF --qt-mode 역방향 수신
```

`processPosBuffer`에서 `PWM_OUT,PAN=...,TILT=...` 라인 파싱 후 emit.

---

## 5. PwmTransmitter — 하드웨어 PWM 송신

### 5.1 두 가지 송신 모드

| 모드 | 환경변수 값 | 통신 방식 | 대상 |
|------|------------|-----------|------|
| `raspi` (**기본**) | `SFEPS_PWM_MODE=raspi` | TCP 이더넷 | Raspberry Pi |
| `stm` | `SFEPS_PWM_MODE=stm` | UDP 무선 | ESP8266 → STM |

### 5.2 환경변수 설정 (`run_client.ps1`)

```powershell
$env:SFEPS_PWM_MODE = "raspi"          # raspi | stm
$env:SFEPS_PWM_HOST = "192.168.0.100"  # 수신 장치 IP
$env:SFEPS_PWM_PORT = "5566"           # 수신 포트
```

### 5.3 전송 패킷 형식

```
SET_PWM,PAN=<pan_us>,TILT=<tilt_us>\n

예시:
SET_PWM,PAN=1540,TILT=1620\n
```

- PAN / TILT 단위: 마이크로초(μs) 기준 PWM 펄스폭 (500 ~ 2500)

### 5.4 Raspberry Pi 수신 예시 (Python)

```python
import socket

s = socket.socket()
s.bind(("0.0.0.0", 5566))
s.listen(1)
conn, _ = s.accept()
while True:
    data = conn.recv(64).decode().strip()
    if data.startswith("SET_PWM,"):
        parts = dict(p.split("=") for p in data[8:].split(","))
        pan  = int(parts["PAN"])
        tilt = int(parts["TILT"])
        # servo_pan.ChangeDutyCycle(pan / 200.0)
        # servo_tilt.ChangeDutyCycle(tilt / 200.0)
```

### 5.5 ESP8266 수신 예시 (Arduino)

```cpp
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <Servo.h>

WiFiUDP udp;
Servo servoPan, servoTilt;

void setup() {
    WiFi.begin("SSID", "PASSWORD");
    udp.begin(5566);
    servoPan.attach(D1);
    servoTilt.attach(D2);
}

void loop() {
    int sz = udp.parsePacket();
    if (sz > 0) {
        char buf[64]; udp.read(buf, sizeof(buf));
        // "SET_PWM,PAN=1540,TILT=1620"
        int pan = 0, tilt = 0;
        sscanf(buf, "SET_PWM,PAN=%d,TILT=%d", &pan, &tilt);
        servoPan.writeMicroseconds(pan);
        servoTilt.writeMicroseconds(tilt);
    }
}
```

---

## 6. FraudManager — 부정승차 대기큐

### 6.1 동작 로직

```
FRAUD|objectId|...|Y 수신
          │
          ▼
    다른 객체 추적 중?
    ┌──── YES ────┐          ┌──── NO ────┐
    ▼             │          ▼            │
m_fraudQueue      │    fraudDetected()    │
에 대기           │    emit (UI 표시)     │
                  │          │            │
                  │          ▼            │
                  │    fraudAutoTrackRequest(
                  │      "TRACK_START|objectId")
                  │          │
                  │          ▼
                  │    positionManager
                  │    camera_RBF 추적 시작
                  │          │
                  │          ▼
                  │    PWM 계산 → PwmTransmitter
                  │
추적 종료(TRACK_END)
          │
          ▼
    setActiveTrackingId("")
          │
          ▼
    drainFraudQueue()
    → 큐의 첫 번째 항목 처리
```

### 6.2 신규 시그널 / 슬롯

```cpp
// signals
void fraudAutoTrackRequest(const QString &cmd);  // main.cpp에서 positionManager에 연결
void fraudQueueChanged(int pendingCount);        // 대기 중인 건 수 변화

// public slots
void setActiveTrackingId(const QString &id);    // PositionManager와 연동
```

### 6.3 main.cpp 연결 코드

```cpp
// FraudManager 자동 추적 → PositionManager
QObject::connect(&fraudManager, &FraudManager::fraudAutoTrackRequest,
                 [&positionManager](const QString &cmd) {
                     positionManager.sendPositionCommand(cmd);
                 });

// PositionManager 추적 상태 → FraudManager 동기화
QObject::connect(&positionManager, &PositionManager::currentSubscribedIdChanged,
                 [&fraudManager, &positionManager]() {
                     fraudManager.setActiveTrackingId(
                         positionManager.currentSubscribedId());
                 });

// camera_RBF PWM 수신 → PwmTransmitter 전송
QObject::connect(&positionManager, &PositionManager::pwmReceived,
                 &pwmTransmitter, &PwmTransmitter::sendPwm);
```

---

## 7. Z=1200mm 평면 좌표 개념

camera_RBF.cpp에서 PWM을 계산하는 핵심 원리입니다.

```
bbox_cy = sel_rect.y + sel_rect.height * ratio   (ratio 기본값: 0.35)
```

- `ratio=0.35` → bbox 상단에서 **35% 아래 지점** = 사람의 발 위치 근사값
- 이 지점이 **Z=1200mm 평면 (바닥면)** 위의 점이라 가정
- `(bbox_cx, bbox_cy)` 픽셀 좌표 → KalmanBbox2D로 `predict_ms=300ms` 예측 → RBF TPS 보간 → `(PAN_pwm, TILT_pwm)`

```
픽셀 좌표 (u, v)
    └─→ KalmanBbox2D.predict(300ms) → (pred_u, pred_v)
            └─→ rbf_pan.eval(pred_u, pred_v)   = PAN  μs
                rbf_tilt.eval(pred_u, pred_v)  = TILT μs
```

---

## 8. 실행 순서

### 8.1 camera_RBF 먼저 실행 (QT 모드)

```cmd
# Windows (MSVC 빌드 후)
camera_RBF.exe rtsp://192.168.0.84/profile2/media.smp ^
    --qt-mode ^
    --remote-id-host 192.168.0.101 ^
    --remote-id-port 5565 ^
    --ratio 0.35 ^
    --predict-ms 300
```

### 8.2 Qt 클라이언트 실행

```powershell
cd C:\Users\2-16\Desktop\SFEPS\client_msvc
.\run_client.ps1
```

> `run_client.ps1`에서 `SFEPS_PWM_MODE`, `SFEPS_PWM_HOST`, `SFEPS_PWM_PORT`를 설정합니다.

### 8.3 Raspberry Pi에서 PWM 수신 대기

```bash
# Raspberry Pi
python3 pwm_receiver.py   # port 5566 TCP 수신
```

---

## 9. 포트 요약

| 포트 | 방향 | 내용 |
|------|------|------|
| 5555 | Qt → 서버 | 로그인 인증 |
| 5557 | 서버 → Qt | FRAUD 알림 (FraudManager) |
| 5558 | 서버 → Qt | 객체 위치 메타데이터 (PositionManager) |
| **5565** | **Qt → camera_RBF** | **TRACK_START / TRACK_END 명령** |
| **5565** | **camera_RBF → Qt** | **PWM_OUT 역방향 전송 (--qt-mode)** |
| **5566** | **Qt → Raspi/ESP8266** | **SET_PWM 하드웨어 전송** |

---

## 10. 수정된 파일 목록

| 파일 | 변경 유형 | 주요 내용 |
|------|-----------|-----------|
| `Camera/get_metadata/src/camera_RBF.cpp` | 수정 | `--qt-mode` 플래그, 클릭 무시, PWM 역방향 소켓 전송 |
| `client_msvc/src/positionmanager.h` | 수정 | `pwmReceived(int, int)` 시그널 추가 |
| `client_msvc/src/positionmanager.cpp` | 수정 | `PWM_OUT` 파싱, TRACK_START bbox 자동 포함 |
| `client_msvc/src/pwmtransmitter.h` | **신규** | Raspberry Pi TCP / ESP8266 UDP 두 가지 모드 |
| `client_msvc/src/pwmtransmitter.cpp` | **신규** | PwmTransmitter 구현 |
| `client_msvc/src/fraudmanager.h` | 수정 | 대기큐 구조체, `setActiveTrackingId`, `fraudAutoTrackRequest` |
| `client_msvc/src/fraudmanager.cpp` | 수정 | `processFraud`, `drainFraudQueue`, `setActiveTrackingId` 구현 |
| `client_msvc/src/main.cpp` | 수정 | PwmTransmitter 등록, 시그널 3개 연결 |
| `client_msvc/CMakeLists.txt` | 수정 | `pwmtransmitter.cpp/.h` 소스 추가 |
| `client_msvc/run_client.ps1` | 수정 | PWM 환경변수 블록 추가 |
