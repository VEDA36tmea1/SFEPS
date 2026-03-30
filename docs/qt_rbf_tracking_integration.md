# Qt 클라이언트 × camera_RBF 통합 구조 문서

> 최종 수정: 2026-03-30  
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
│   RTSP stream    ──→ Qt client 내 videoBackend               │
│   ONVIF metadata ──→ videoBackend → rbfqt_process_metadata  │
└─────────────────────────────────────────────────────────────────┘
              │ (bbox/ID 리스트를 매 tick 갱신)
              ▼
┌─────────────────────────────────────────────────────────────────┐
│ Qt 클라이언트 (client_msvc)                                    │
│                                                                 │
│  FraudManager   ──→ FRAUD 수신 (port 5557)                      │
│      ├─ isFraud면 m_fraudXmlIds에 저장 → 빨간 bbox UI 표시    │
│      └─ fraudAutoTrackRequest → main.cpp → videoBackend.trackByXmlId
│                                                                 │
│  [Track 버튼 클릭]                                             │
│    ├─ PositionManager.sendPositionCommand("TRACK_START|id")  │
│    └─ videoBackend.trackByNativeId(id) → m_manualTracking=true │
│                                                                 │
│  onPwmTick() (33ms)                                            │
│    ├─ 자동 모드: sticky(xml/stable) 기반 fraud 후보 선택     │
│    ├─ rbfqt_set_tracked_nativeid(bestKey) 결정               │
│    └─ rbfqt_compute_pwm() → pwmSetRequested(pan,tilt)          │
│                                                                 │
│  pwmSetRequested → PwmTransmitter.sendPwm(pan,tilt) (+ PositionManager SET_PWM) │
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

### 3.1 Qt 모드(in-process) 진입 방식

현재 Windows Qt 클라이언트는 `camera_RBF.cpp`를 별도 `camera_RBF.exe`로 실행하지 않고,
클라이언트 내부에서 `rbfqt_init()`/`rbfqt_process_metadata()`/`rbfqt_compute_pwm()`을 호출해서 PWM을 계산합니다.

주요 호출/진입점:
- `client_msvc/src/videobackend.cpp`
  - 초기화: `rbfqt_init(m_pwmRatio, m_pwmAlpha, m_predictMs, ...)`
  - tick 계산: `MainWindow::onPwmTick()`에서 `rbfqt_compute_pwm(...)`

### 3.2 Qt mode 핵심 API

`camera_RBF.cpp` Qt 모드 핵심은 아래처럼 동작합니다.

1. `rbfqt_process_metadata(humans, W, H)`
   - NativeTrack bbox 갱신
   - IdStabilizer(re_id) 업데이트 → `S_xxx` stable ID 매핑 갱신

2. 자동/수동 추적 대상 지정
   - `rbfqt_set_tracked_nativeid(bestKey)` 또는
   - `rbfqt_set_target_bbox(l,t,r,b, W,H)` (fallback bbox 사용 시)

3. PoseAim 오버라이드(선택)
   - `rbfqt_set_pose_aim(u_px, v_px, valid)`
   - `valid==1`이면 `rbfqt_compute_pwm()` 내부에서 bbox_cx/bbox_cy를 pose u/v로 대체

4. `rbfqt_compute_pwm(now, W, H, &pan, &tilt)`
   - tracked bbox 기준 Kalman predict (`predict_ms = SFEPS_RBF_PREDICT_MS`)
   - RBF(TPS) 보간 → PAN/TILT PWM 계산

### 3.3 관련 설정(환경변수)

Qt/클라이언트에서는 아래 값들이 직접 사용됩니다.
- `SFEPS_RBF_PREDICT_MS`: Kalman predict 시간 보정
- `SFEPS_POSE_DOWN_RATIO`: pose 기반 v 오프셋 비율

---

## 4. PositionManager — TRACK_START bbox 자동 포함

### 4.1 동작
현재 Qt client에서 `PositionManager`는 주로 **수동 Track** 시 서버 구독을 시작/종료하는 용도로 사용됩니다.

`sendPositionCommand("TRACK_START|id")` 호출 시, 내부 `m_pendingMap`에서 최근 bbox를 찾아 **전송 포맷에 L/T/R/B를 자동 포함**합니다.

```
Qt 입력:        TRACK_START|N1
실제 전송:      TRACK_START|N1|L=120|T=50|R=300|B=800
```

### 4.2 목적 — ID 매칭 불일치 보완

Qt의 native tracker와 camera_RBF.cpp의 native tracker는 독립 동작합니다.  
같은 ONVIF 메타데이터를 파싱하지만 타이밍 차이로 `N1`이 서로 다른 사람일 수 있습니다.

클라이언트 내부 rbfqt tracker(native tracker)는 ID 매칭 실패 시 전달된 bbox 좌표로 **IoU 매칭을 시도**합니다.

```
client rbfqt 내부 매칭 순서:
  1. g_selected_id == obj.id  → 완전 일치
  2. raw_objs에서 ID 검색     → DeepSORT 안정화 전 원본
  3. has_remote_bbox → IoU 매칭 (bbox 좌표 기반) ← bbox 포함 시 여기서 복구
```

### 4.3 `pwmReceived` 시그널 (신규)

```cpp
// positionmanager.h
signals:
    void pwmReceived(int pan, int tilt);  // legacy 호환용(현재는 in-process PWM 사용)
```

`processPosBuffer`에서 `PWM_OUT,PAN=...,TILT=...` 라인 파싱 후 emit합니다.

다만 현재 Qt client 통합 빌드에서는 PWM은 `MainWindow::pwmSetRequested`를 통해 `PwmTransmitter`로 직접 전송하므로,
`pwmReceived`는 호환/레거시 경로로 보시면 됩니다.

---

## 5. PwmTransmitter — 하드웨어 PWM 송신

### 5.1 두 가지 송신 모드

| 모드 | 환경변수 값 | 통신 방식 | 대상 |
|------|------------|-----------|------|
| `raspi` (**기본**) | `SFEPS_PWM_MODE=raspi` | TCP 이더넷 | Raspberry Pi |
| `stm` | `SFEPS_PWM_MODE=stm` | UDP 무선 | ESP8266 → STM |

### 5.2 환경변수 설정 (`run_client.ps1`)

```powershell
$env:SFEPS_PWM_MODE = "raspi"     # "raspi" | "stm"
$env:SFEPS_PWM_HOST = "<ip>"     # Raspi(TCP 목적지) 또는 STM(UDP 목적지) IP
$env:SFEPS_PWM_PORT = "<port>"   # Raspi/TCP 또는 STM/UDP 목적지 포트
```

### 5.3 전송 패킷 형식

```
SET_PWM,PAN=<pan_us>,TILT=<tilt_us>\n

예시: `SET_PWM,PAN=1540,TILT=1620\n`
```

- Qt `PwmTransmitter::sendPwm()`는 `\n` 포함한 위 문자열을 그대로 전송합니다.
- `pan_us`, `tilt_us` 단위: 마이크로초(μs) 기준 PWM 펄스폭 (클라이언트 기본 pan/tilt 범위: `SFEPS_PWM_PAN_MIN/MAX`, `SFEPS_PWM_TILT_MIN/MAX`)

### 5.4 Raspberry Pi 수신 예시 (Python)

`hardware/Raspi-laser/set_pwm_server.py` 실행:

```bash
python3 set_pwm_server.py --port <SFEPS_PWM_PORT> -v
```

이 서버는 TCP로 Qt client의 연결을 받고,
`\n` 단위로 `SET_PWM,PAN=...,TILT=...` 라인을 파싱해서
GPIO12(PAN), GPIO13(TILT) PWM을 갱신합니다.

기본 clamp 값은 `--min-us=800`, `--max-us=2200` 입니다.

### 5.5 STM(ESP8266/STM32 등) 수신 방식 (UDP)

`stm` 모드에서는 Qt client가 `QUdpSocket::writeDatagram()`로
`SET_PWM,PAN=...,TILT=...\n` 문자열을 `<SFEPS_PWM_HOST>:<SFEPS_PWM_PORT>`로 전송합니다.

STM 쪽은 다음만 만족하면 됩니다.
- UDP 수신 소켓을 `<SFEPS_PWM_PORT>`에 bind
- UDP payload 문자열에서 `PAN=` / `TILT=` 값을 파싱 (payload 끝의 `\n` 무시 가능)
- 파싱된 `pan_us`, `tilt_us`로 PWM(서보/레이저 드라이버) 갱신

---

## 6. FraudManager — 부정승차 대기큐

### 6.1 동작 로직

```
FRAUD|objectId|...|Y 수신
          │
          ▼
fraudDetected(objectId, ..., isFraud=true) emit
          │
          ├─ main.cpp에서 isFraud면 videoBackend.addFraudXmlId(objectId)
          │     → m_fraudXmlIds에 저장 → 빨간 bbox UI 표시
          │
          ▼
isFraud && (activeTrackingId가 비어있음 또는 동일)?
          │
          ├─ YES:
          │    m_activeTrackingId = objectId
          │    fraudAutoTrackRequest(objectId, bboxL, bboxT, bboxR, bboxB) emit
          │      → main.cpp에서 videoBackend.trackByXmlId(xmlId, fallback bbox)
          │          (stickyXml/stickyStable 등록 + rbfqt target 설정)
          │
          └─ NO:
               m_fraudQueue에 대기열로 저장 후 return

추적 종료(레이저 stop 또는 수동 Untrack)
          │
          ▼
videoBackend.clearRbfTarget()에서 trackingXmlIdChanged("") emit
          │
          ▼
main.cpp → fraudManager.setActiveTrackingId("") 호출
          │
          ▼
FraudManager가 큐를 drain하며 다음 fraudAutoTrackRequest 수행
```

### 6.2 신규 시그널 / 슬롯

```cpp
// signals
void fraudAutoTrackRequest(const QString &xmlId,
                            float bboxL, float bboxT, float bboxR, float bboxB);
void fraudQueueChanged(int pendingCount);        // 대기 중인 건 수 변화

// public slots
void setActiveTrackingId(const QString &id);
```

### 6.3 main.cpp 연결 코드

```cpp
// isFraud면 빨간 bbox 표시용으로 xmlId 저장
QObject::connect(&fraudManager, &FraudManager::fraudDetected,
                 [&videoBackend](const QString &objectId,
                                 const QString & /*cardAgeText*/,
                                 const QString & /*age*/,
                                 bool isFraud,
                                 const QString & /*tag*/,
                                 const QString & /*imagePath*/) {
                     if (isFraud && !objectId.isEmpty())
                         videoBackend.addFraudXmlId(objectId);
                 });

// 자동 추적: sticky target 등록 + rbfqt target 설정
QObject::connect(&fraudManager, &FraudManager::fraudAutoTrackRequest,
                 [&videoBackend](const QString &xmlId,
                                 float bboxL, float bboxT,
                                 float bboxR, float bboxB) {
                     videoBackend.trackByXmlId(xmlId, bboxL, bboxT, bboxR, bboxB);
                 });

// tracking 상태 변경 → FraudManager 큐 drain 트리거
QObject::connect(&videoBackend, &MainWindow::trackingXmlIdChanged,
                 [&fraudManager](const QString &xmlId) {
                     fraudManager.setActiveTrackingId(xmlId);
                 });

// 레이저 stop(자동 종료) → 서버 subscription 종료(TRACK_END)
QObject::connect(&videoBackend, &MainWindow::laserTrackStopped,
                 [&positionManager](const QString &xmlId) {
                     if (xmlId.isEmpty()) return;
                     positionManager.sendPositionCommand(QStringLiteral("TRACK_END|%1").arg(xmlId));
                 });
```

---

## 7. Qt 자동 레이저 대상 선택/유지 (MainWindow::onPwmTick)

`MainWindow::onPwmTick()`은 **33ms 타이머**로 동작하며, 자동 모드에서는 아래 로직으로 레이저 타겟을 유지/전환합니다.

### 7.1 수동 Track 켠 경우(우선순위)

- 수동 Track: QML `Track` 버튼이 `videoBackend.trackByNativeId()`를 호출
- `trackByNativeId()`는 `m_manualTracking=true`로 설정
- 서버에서 새 FRAUD가 와도 `videoBackend.trackByXmlId()`는 `m_manualTracking`일 때 **무시**(덮어쓰기 방지)

따라서 “수동 track on 상태에서 더 작은 객체로 레이저가 넘어가는 현상”을 차단합니다.

### 7.2 자동 모드: sticky 기준으로 최솟값(작은 객체) 선택

자동 모드에서는 다음 스냅샷을 로드합니다.
- `stickyXml` = `m_fraudLaserStickyXmlId` (ONVIF XML ID)
- `stickyStable` = `m_fraudLaserStickyStableId` (IdStabilizer `S_xxx`)
- `m_fraudXmlIds` 기반으로 `fraud=true` 후보 리스트를 구성

후보 선택(bestKey):
- `safeArea = (w*h>0)?(w*h):1e-6`로 0면적 예외 처리
- `kSwitchRatio = 0.85`: 새 후보의 면적이 현재보다 충분히 작을 때만 전환
- occlusion guard:
  - `kMaxIouBlock = 0.25`
  - `IoU(sticky bbox, best bbox) > 0.25` 이면 전환 억제

### 7.3 “stickyStableId 우선, 없으면 xml fallback” 정책

- `stickyStable`이 비어 있지 않으면:
  - 존재/전환 판단/후보 선택은 **stable ID(S_xxx) 기준만** 허용
  - stable 매핑이 없는 후보는 best 후보에서 제외
- `stickyStable`이 비어 있으면:
  - 기존처럼 `stickyXml`(XML ID) 기준으로 fallback

전환이 일어나면 `m_fraudLaserStickyXmlId`와 `m_fraudLaserStickyStableId`를 함께 갱신합니다.

### 7.4 stop 조건(실종 허용)

- fraud 후보(bestKey)가 없는 동안:
  - sticky가 화면에 보이지 않는 상태로 간주하고
  - `kMaxMissingFrames = 10`을 넘기면 stop
- stop 시:
  - `laserTrackStopped(stickyXml)` emit
  - `clearRbfTarget()`로 다음 fraud 큐 drain이 자연스럽게 이어지게 queued cleanup 수행

---
## 8. Pose Aim 오버라이드(어깨 중심 아래 타겟)

PoseAim은 “bbox_cx/bbox_cy” 대신 **MediaPipe Pose 기반 u/v 조준점**을 주입하는 기능입니다.

### 8.1 onPwmTick에서 pose 요청/주입

- pose 요청 주기: `kPoseEveryTicks=5` (33ms 기준 약 165ms)
- stale 허용: `kPoseStaleMs=800ms`
- crop padding: `kPosePadRatio=0.15`
- jpeg quality: `kPoseJpegQuality=80`
- 세로 오프셋/비율: `m_poseDownRatio` (환경변수 `SFEPS_POSE_DOWN_RATIO`로 설정)

pose 결과가 staleOk면:
- `rbfqt_set_pose_aim(aimU, aimV, 1)`로 주입
stale이면:
- `rbfqt_set_pose_aim(0, 0, 0)`로 clear(valid=false)

### 8.2 camera_RBF.cpp 반영 지점

`camera_RBF.cpp`의 `rbfqt_compute_pwm()`은 `g_rbfqt_poseAimValid==true`이면
`bbox_cx/bbox_cy` 계산을 pose aim u/v로 대체한 뒤 Kalman + RBF를 수행합니다.

---
## 9. Z=1200mm 평면 좌표 개념(기본 bbox target)

PoseAim이 유효하지 않으면, 기본 target은 아래 비율 지점입니다.

```
bbox_cy = sel_rect.y + sel_rect.height * ratio   (ratio 기본값: 0.35)
```

- `ratio=0.35` → bbox 상단에서 **35% 아래 지점** = 발 위치 근사값
- `(bbox_cx, bbox_cy)` 픽셀 좌표 → KalmanBbox2D로 `predict_ms = SFEPS_RBF_PREDICT_MS` 예측
- 예측 결과를 RBF TPS로 보간 → `PAN/TILT` PWM 계산

---
## 10. 실행 순서(run_client.ps1 기준)

### 10.1 Windows Qt client 실행

```powershell
cd C:\Users\2-16\Desktop\SFEPS\client_msvc
.\run_client.ps1
```

`run_client.ps1`는 환경변수를 설정한 뒤 `appHanwhaVisionSFEPS.exe`를 실행합니다.
- `SFEPS_RBF_PREDICT_MS`
- `SFEPS_POSE_DOWN_RATIO`
- `SFEPS_PWM_MODE`, `SFEPS_PWM_HOST`, `SFEPS_PWM_PORT`

### 10.2 Raspberry Pi PWM 수신

```bash
python3 pwm_receiver.py   # TCP 5566 수신
```

---
## 11. 포트 요약

| 포트 | 방향 | 내용 |
|------|------|------|
| 5555 | Qt → 서버 | 로그인 인증(Auth) |
| 5557 | 서버 → Qt | FRAUD 알림(FraudManager) |
| 5558 | 서버 → Qt | 위치 스트림/오브젝트 POS(overlay/monitoring 용도) |
| 5566 | Qt → Raspi/ESP8266 | SET_PWM 하드웨어 전송 |

※ `5565`/`--qt-mode` 역방향 소켓은 별도 `camera_RBF.exe` 실행 시나리오에서만 해당하며,
현재 Windows Qt client(in-process 통합)에서는 사용하지 않습니다.

---
## 12. 수정된 파일 목록

| 파일 | 변경 유형 | 주요 내용 |
|------|-----------|-----------|
| `Camera/get_metadata/src/camera_RBF.cpp` | 수정 | `rbfqt_*` Qt API의 stable 매핑/pose aim/계산 경로 |
| `client_msvc/src/videobackend.cpp` | 수정 | onPwmTick 자동 스위칭( stickyStableId 우선 ) + PoseAim 주입 + 수동 override 가드 |
| `client_msvc/src/fraudmanager.h/.cpp` | 수정 | fraud queue/auto track request emission |
| `client_msvc/src/main.cpp` | 수정 | FraudManager → videoBackend 연결 및 laser stop 시 TRACK_END 전송 |
| `client_msvc/src/pwmtransmitter.h/.cpp` | 신규 | Raspberry Pi TCP / ESP8266 UDP 두 가지 모드 |
| `client_msvc/run_client.ps1` | 수정 | `SFEPS_RBF_PREDICT_MS`, `SFEPS_POSE_DOWN_RATIO` 등 환경변수 |
