## Camera/get_metadata 개발 정리 (2026-03-12)

### 빌드 가이드 (Ubuntu + Windows 동시 지원)

이 폴더는 현재 두 가지 빌드 경로를 모두 지원한다.

- **Ubuntu/Linux:** 기존 `Makefile` 사용 (`g++`, `pkg-config opencv4`)
- **Windows/MSVC:** `CMakeLists.txt` 사용 (`Visual Studio 2022`, OpenCV vc16)

중요:
- `x64/mingw/lib` 는 **MinGW g++** 와 맞는 OpenCV 라이브러리 경로다.
- `x64/vc16/lib` 는 **MSVC cl** 과 맞는 OpenCV 라이브러리 경로다.
- 컴파일러 ABI가 다르므로 교차 링크(예: g++ + vc16)는 불가하다.

#### Ubuntu/Linux 빌드

```bash
cd ~/Desktop/SFEPS/Camera/get_metadata
make camera_RBF
./camera_RBF
```

#### Windows/MSVC 빌드

```powershell
cd C:\Users\2-16\Desktop\SFEPS\Camera\get_metadata
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64 -DOpenCV_DIR="C:/Users/2-16/Downloads/opencv/build"
cmake --build build-msvc --config Release --target camera_RBF
.\build-msvc\Release\camera_RBF.exe
```

실행 시 DLL 경로 필요:

```powershell
$env:Path = "C:\Users\2-16\Downloads\opencv\build\x64\vc16\bin;$env:Path"
```

### 1. 개요

이 폴더는 ONVIF 카메라에서 **RTSP + 메타데이터(XML)** 를 받아서:

- 객체(Human 등)의 위치/바운딩 박스를 파싱하고
- 사람이 서 있는 위치(foot point, bottom-Y)를 기준으로 좌표를 얻어
- 향후 **레이저/서보 제어, world-track 좌표계**와 연동하기 위한 실험용 도구를 모아둔 곳이다.

기존에는 `main.cpp + RTSPClient + XMLParser` 로 **꼬리물기(tailgating)** 분석/로그 중심이었고,  
이번에 사람 바운딩 박스를 시각적으로 확인하고, 클릭해서 좌표를 얻을 수 있는 `camera_client`를 추가했다.

---

### 2. 주요 구성 요소

#### 2.1 RTSPClient

파일: `inc/RTSPClient.h`, `src/RTSPClient.cpp`

- 기능:
  - 카메라에 TCP로 접속 (`CAMERA_IP`, `CAMERA_PORT`)
  - RTSP 핸드쉐이크:
    - OPTIONS
    - DESCRIBE
    - SETUP (Video track, Metadata track)
    - PLAY
  - 세션 유지용 `sendHeartbeat()` (주기적인 GET_PARAMETER)
- 사용:
  - `connectToCamera()` → `sendHandshake()` → 이후 `getSocket()`으로 TCP 소켓 fd 획득

#### 2.2 XMLParser

파일: `inc/XMLParser.h`, `src/XMLParser.cpp`

- **기존 기능** (`parseAndProcess`):
  - 누적된 XML 스트링에서 `<tt:Object>` / `<wsnt:NotificationMessage>` 를 파싱
  - `obj_type == "Human"` 인 객체의 (x,y,left,right,top,bottom) 및 속도를 추적
  - ID 끊김을 벡터(속도/관성) 기반으로 복구하는 로직 (tracking_map)
  - 특정 RuleName(Event)에 대해 tailgating 감지/로그 출력

- **추가된 분석용 API** (2026-03-12):

  ```cpp
  struct ParsedMetadataObject {
      std::string id;
      std::string type;
      float x;
      float y;
      float left;
      float top;
      float right;
      float bottom;
  };

  std::vector<ParsedMetadataObject>
  XMLParser::parseHumanObjectsForAnalytics(const std::string& xml,
                                           bool detect_all = false) const;
  ```

  - XML 블록에서 `<tt:Object>` 태그를 스캔하여, **Human 객체의 바운딩 박스와 중심 좌표를 구조체로 반환**
  - 인자 `detect_all`:
    - `false`(기본): `type == "Human"` 인 객체만 반환 (기존 동작과 동일)
    - `true`: Human 이 아닌 타입(예: Vehicle 등)도, 좌표가 유효하면 모두 반환

---

### 3. `camera_client` (사람 바운딩 박스 클릭용 클라이언트)

파일: `src/camera_client.cpp`  
빌드: `Makefile` 에 `CAM_TARGET = camera_client` 추가 (2026-03-12)

#### 3.1 역할

- RTSPClient 로 **메타데이터 트랙(XML)** 수신
- `XMLParser::parseHumanObjectsForAnalytics` 로 **Human (또는 모든 타입)** 바운딩 박스 파싱
- OpenCV `VideoCapture(RTSP_URL)` 로 **실제 영상 프레임** 수신
- 영상 창에 **객체 바운딩 박스 그리기**
- 마우스로 사람 박스를 클릭하면:
  - 해당 박스의 **center X 픽셀** + **bottom Y 픽셀(발 위치)** 를 계산
  - 아래 포맷으로 stdout에 한 줄 출력:

    ```text
    HUMAN_BOTTOM id=<id> type=<Type> center_x_px=<cx> bottom_y_px=<by>
    ```

  - 이후 `ubuntu_tcp_server` 나 다른 프로세스에 파이프로 연결해서 활용 가능.

#### 3.2 실행 방법

```bash
cd /home/ros2man/Desktop/SFEPS/Camera/get_metadata

# camera_client만 빌드해서 실행 (영상 + 객체 바운딩 박스 확인용)
make camera_client

./camera_client                 # 기본: Human 타입만 표시
./camera_client --detect-all    # Human 외 모든 타입 객체도 박스로 표시
./camera_client --pose-off      # MediaPipe Pose 오버레이 비활성화
./camera_client --pose-print    # pose 오버레이는 켜고, 핵심 키포인트 픽셀 좌표를 stdout에 출력
```

실행 후:

- 창 이름: `camera_client`
- 박스 색: 노란색 (`cv::Rect(left,top,width,height)`)
- 마우스 **왼쪽 클릭**:
  - 클릭한 픽셀이 포함된 박스를 찾아서
  - 박스 가장 아래 점(bottom Y) 기준으로 빨간 점 찍고
  - stdout 으로 `HUMAN_BOTTOM ...` 라인 출력
- 클릭한 박스는 **선택(SEL)** 상태가 되며, Pose 추정은 **선택된 객체 bbox(ROI)** 에서만 수행됨 (빠름)
- `d` 키: **선택 객체 내부**에서 클릭한 픽셀의 정규화 비율 `CLICK_RATIO ... ratio=(rx,ry)` 를 stdout으로 출력 토글
- 종료: `q` 또는 `ESC` 또는 Ctrl+C

#### 3.3 내부 동작 요약

- 메타데이터 스레드:

  ```cpp
  std::thread meta_thread(metadata_thread_fn, &client, &parser);
  ```

  - RTSP interleaved RTP 헤더(`$`, channel, length)를 읽고
  - metadata channel(2)에서 RTP payload 중 XML 부분만 누적
  - RTP timestamp가 바뀔 때마다:

    ```cpp
    auto humans = parser->parseHumanObjectsForAnalytics(accumulated_xml, g_detect_all);
    g_objects = std::move(humans);
    accumulated_xml.clear();
    ```

  - `g_objects` 는 전역 mutex 로 보호되는 **최근 프레임 기준 객체 리스트**

- 영상 루프:
  - `cv::VideoCapture cap(RTSP_URL);`
  - 매 프레임마다 `g_last_frame` 갱신 + `g_objects` 복사 후 사각형/ID 텍스트 그리기

- 마우스 콜백:

  ```cpp
  cv::setMouseCallback("camera_client", on_mouse, &g_last_frame);
  ```

  - 클릭 좌표 (x,y)가 현재 프레임 기준 어떤 박스 안에 있는지 검사
  - 포함되면:
    - `bottom_y_px = rect.y + rect.height`
    - `center_x_px = rect.x + rect.width / 2`
    - 위 값과 id/type을 stdout으로 출력

---

### 4. Makefile 변경 (2026-03-12)

파일: `Makefile`

- 기존: `app` 하나만 빌드
- 변경:

  - `CAM_SRCS = src/camera_client.cpp src/RTSPClient.cpp src/XMLParser.cpp`
  - `CAM_TARGET = camera_client`
  - `OPENCV_FLAGS = \`pkg-config --cflags --libs opencv4\``

```make
all: $(TARGET) $(CAM_TARGET)

$(CAM_TARGET): $(CAM_SRCS)
	$(CXX) -std=c++17 -Wall -I./inc $(CAM_SRCS) -o $(CAM_TARGET) $(OPENCV_FLAGS)
```

이로써 `make` 한 번으로:

- `app` (기존 tailgating 분석 도구)
- `camera_client` (사람 바운딩 박스 클릭 + bottom-Y 좌표 출력용)

두 실행 파일을 동시에 빌드할 수 있다.

---

### 5. `camera_RBF` — RBF 보간 기반 레이저/서보 PWM 제어 (2026-03-19)

파일: `src/camera_RBF.cpp`  
빌드: `make camera_RBF` 또는 `make` (전체 빌드)

#### 5.1 개요

`camera_client`의 "클릭→좌표 출력" 기능을 확장하여, ONVIF 메타데이터에서 얻은 사람 bbox 위치를 **RBF(Thin-Plate Spline) 보간**으로 PWM 값으로 변환하고, **칼만 필터**로 이동 방향을 예측하여 레이저/서보를 제어하는 파이프라인이다.

```
camera_RBF (stdout) ──pipe──▶ ubuntu_tcp_server ──TCP──▶ Raspberry Pi (set_pwm_client.py)
```

#### 5.2 주요 구현 사항

##### RBF (Thin-Plate Spline) 2D 보간

- 16개 캘리브레이션 포인트 `(u, v) → (pan_us, tilt_us)` 를 하드코딩  
- Thin-Plate Spline 커널 `φ(r) = r² log(r + ε)` 을 사용한 보간  
- SVD 분해로 가중치 w + affine 파라미터 (a0, a1, a2) 계산  
- 임의의 픽셀 좌표 `(u, v)` → PWM `(pan, tilt)` 실시간 변환  
- (옵션) 월드 좌표 `(X_cm, Y_cm)` → 픽셀 `(u, v)` RBF로 1200mm 평면 그리드 오버레이

##### 실시간 객체 추적

- 마우스 클릭으로 선택한 객체의 ID를 기억
- 매 프레임 ONVIF 메타데이터에서 **같은 ID의 최신 bbox**를 조회하여 갱신
- 선택 bbox의 `center_x`, `top + height × ratio` 지점을 타겟 좌표로 사용
- 선택한 객체가 없을 때는 `SET_PWM` 미전송 (빈 줄 heartbeat만 전송하여 파이프 건강 체크)

##### 칼만 필터 (alpha-beta) 300ms 예측 (2026-03-19 추가)

이동하는 사람의 위치를 추적하고, 레이저가 도달하기까지의 지연(~300ms)을 보상하기 위해 2D 칼만 필터를 적용했다.

| 파라미터 | 기본값 | 역할 |
|---|---|---|
| `alpha_pos` | 0.6 | 위치 보정 비율 (0~1, 클수록 측정값에 민감) |
| `beta_vel` | 0.15 | 속도 보정 비율 (0~1, 클수록 속도 변화에 민감) |
| `alpha_size` | 0.3 | bbox 크기 exponential smoothing 비율 |
| `predict_ms` | 300 | 미래 예측 시간 (ms), CLI `--predict-ms`로 조절 |

동작 흐름:

1. 매 프레임 측정값 `(bbox_cx, bbox_cy)` + `dt`로 필터 갱신  
2. `predict(0.3초)` → 예측 좌표 `(pred_u, pred_v)` 계산  
3. 예측 좌표로 RBF → PWM 변환 → `SET_PWM,PAN=...,TILT=...` 출력  
4. 새 객체 선택 시 필터/스무딩 즉시 리셋 (이전 값에 끌리지 않음)

##### 파이프라인 자동 종료

- `SIGPIPE` 핸들러: 다운스트림 프로세스(`ubuntu_tcp_server`) 종료 시 자동 종료
- `std::cout` 상태 체크: 매 프레임 stdout이 유효한지 확인, 깨지면 `g_running = false`
- heartbeat: 선택 객체 없을 때 30프레임마다 빈 줄 전송 → 파이프 파손 조기 감지

#### 5.3 실행 방법

```bash
cd /home/ros2man/Desktop/SFEPS/Camera/get_metadata
make camera_RBF

# 기본 실행 (단독, 화면에서 확인만)
./camera_RBF

# ubuntu_tcp_server와 파이프 연결 (라즈베리 제어)
./camera_RBF | ../hardware/stm32-laser/tmp_server/ubuntu_server/ubuntu_tcp_server --tcp 5555
```

#### 5.4 CLI 옵션

| 옵션 | 기본값 | 설명 |
|---|---|---|
| `--detect-all` | off | Human 외 모든 객체 타입도 표시 |
| `--ratio <f>` | 0.35 | bbox 상단으로부터의 타겟 비율 (0=상단, 1=하단) |
| `--alpha <f>` | 0.5 | PWM 스무딩 계수 (0=변화 없음, 1=즉시 반영) |
| `--send-every <n>` | 1 | n프레임마다 SET_PWM 전송 (프레임 스킵) |
| `--predict-ms <f>` | 300 | 칼만 필터 예측 시간 (ms). 0이면 예측 비활성화 |
| `--no-grid` | off | 1200mm 평면 그리드 오버레이 비활성화 |

#### 5.5 화면 시각화 요소

| 요소 | 색상 | 의미 |
|---|---|---|
| 노란색 사각형 | `(0,255,255)` | ONVIF 메타데이터 바운딩 박스 |
| 초록색 사각형 | `(0,255,0)` | 현재 선택(추적 중)인 객체의 bbox |
| 초록 수평선 | `(0,255,0)` | bbox 내 ratio 지점 (타겟 높이) |
| 주황색 작은 점 | `(0,165,255)` | 현재 측정된 타겟 위치 |
| 마젠타 큰 점 | `(255,0,255)` | 300ms 예측 위치 (PWM이 이 좌표 기준) |
| 마젠타-주황 연결선 | `(255,0,255)` | 예측 방향/거리 |
| 상단 텍스트 | `(0,255,255)` | `src`, `predict`, `pan`, `tilt`, `vx`, `vy` 정보 |

#### 5.6 출력 포맷

stdout (`ubuntu_tcp_server`로 파이프):

```
SET_PWM,PAN=1234,TILT=1350
```

stderr (디버그 로그):

```
CLICK_PWM id=596948 meas=(512,400) pred=(530,395) vel=(58.3,-16.2) PAN=1180 TILT=1370
[FPS] 29.8
```

---

### 6. 2026-03-24 업데이트 (Windows/GStreamer/Tracker 모드)

#### 6.1 Windows 빌드 체계 정리

- Windows에서 `Makefile` 기준 **MSVC(cl) + OpenCV vc17** 빌드로 정리됨
- 기본 OpenCV 루트: `C:/Users/2-16/Downloads/opencv-gst/install`
- 라이브러리/런타임 경로:
  - `x64/vc17/lib`
  - `x64/vc17/bin`
- 실행 헬퍼 `make run-rbf`는 OpenCV/GStreamer PATH를 자동으로 설정

자세한 절차는 `Window_build.md` 참고.

#### 6.2 camera_RBF 수신 경로 저지연화

- 수신은 별도 캡처 스레드에서 **최신 프레임만 유지**(old frame overwrite)
- OpenCV 캡처 오픈 순서:
  1) GStreamer UDP
  2) GStreamer TCP
  3) 기본 backend fallback
- GStreamer 파이프라인은 `appsink sync=false max-buffers=1 drop=true` 저지연 옵션 사용

#### 6.3 tracker 모드 전환 추가

`Camera/get_metadata`의 `camera_RBF`는 트래커 모드를 인자로 전환할 수 있습니다.
아래 명령은 `tmp_client`가 아니라 `Camera/get_metadata` 폴더에서 실행합니다.

```powershell

# 기본 실행(현재 기본값)
make run-rbf

# C++ 네이티브 트래커 모드
make run-rbf-native

# DeepSORT 모드
make run-rbf-deepsort
```

직접 인자 전달도 가능합니다:

```powershell
make run-rbf ARGS="--tracker-mode native --predict-ms 200"
make run-rbf ARGS="--tracker-mode deepsort --predict-ms 200"
```

#### 6.4 ONVIF 메타데이터 XML 확인 (`dump_metadata_xml`)

`XMLParser`는 **XML 문자열**만 넘기면 단독으로 동작한다. 카메라에서 오는 원시 XML을 보려면 `dump_metadata_xml`을 쓰면 된다 (OpenCV 불필요).

빌드:

```powershell
cd C:\Users\2-16\Desktop\SFEPS\Camera\get_metadata
make dump_metadata_xml.exe
```

- **실시간**: RTSP 메타 트랙(channel 2)에서 RTP 타임스탬프가 바뀔 때마다 누적된 XML을 stdout에 출력하고, 이어서 `parseHumanObjectsForAnalytics(..., detect_all=true)` 결과를 출력한다.

```powershell
.\dump_metadata_xml.exe
.\dump_metadata_xml.exe --max-frames 3
```

- **오프라인**: 저장해 둔 XML 파일로 동일 파서 결과 확인 (`--detect-all`이면 `Human` 외 타입도 포함).

```powershell
.\dump_metadata_xml.exe --file saved_meta.xml
.\dump_metadata_xml.exe --file saved_meta.xml --detect-all
```

카메라는 보통 `<tt:Object>` 블록을 **여러 개**(예: Human 전신 + Head) 보내므로, 원시 XML에서 `ObjectId`, `<tt:Type>...</tt:Type>`를 직접 보면 구조를 확인할 수 있다.

---




#### 6.5 native 모드 개선 포인트

- IoU + 중심거리 기반 매칭
- 겹침(crowded) 구간에서 게이트 강화로 ID 스위치 억제
- 직전 매칭 det lock으로 교차 구간 ID 뒤바뀜 완화
- 정지 상태 adaptive smoothing:
  - center/size alpha 자동 하향
  - size deadband 및 aspect-ratio jump guard
  - bbox width/height 펌핑(늘어남/찝힘) 억제


### 7. IdStabilizer (re_id) — 후처리 ID 복원 모듈 (2026-03-26)

파일: `inc/re_id.h`, `src/re_id.cpp`

#### 7.1 배경

ONVIF AI 카메라는 사람을 탐지할 때 `ObjectId`를 부여하지만, 가림(occlusion)이나 화면 이탈 후 재진입 시 새로운 ID를 발급한다. 카메라 내부 탐지 로직은 수정 불가하므로, 메타데이터를 받아 C++ 후처리로 ID를 복원하는 외부 모듈을 설계했다.

기존 `XMLParser::parseAndProcess()`의 `tracking_map`(IoU + 속도 예측, 5초 타임아웃)을 대체/보강하며, `camera_client`와 `camera_RBF` 양쪽에 적용된다.

#### 7.2 알고리즘

```
XMLParser::parseHumanObjectsForAnalytics()
    ↓ (ParsedMetadataObject → DetectedInput 변환)
IdStabilizer::update()
  ├─ 칼만 필터(alpha-beta)로 각 트랙의 다음 위치 예측
  ├─ 예측 위치 기준 IoU(0.25) + 가우시안 거리(0.55) + 외형(0.20) 가중합
  ├─ 헝가리안 알고리즘으로 전체 최적 1:1 매칭
  ├─ 미매칭 탐지 → 갤러리에서 기존 트랙 복구 시도
  └─ 완전 신규 → 새 stable_id 발급 (S_001, S_002, ...)
    ↓
StableObject (안정 ID + EMA 스무딩된 bbox)
```

##### 칼만 필터

- `alpha_pos=0.7`: 위치 보정 게인 (높을수록 측정값 신뢰)
- `beta_vel=0.3`: 속도 학습 게인 (높을수록 방향 전환에 빠르게 반응)
- outlier 게이팅: bbox 대각선 × 4 까지 허용, 초과 시 위치 리셋 + 방향 힌트 보존
- 속도 상한: 좌표계에 적응 (정규화 ~2.0/s, 픽셀 ~3000px/s)

##### 거리 유사도

- 가우시안 감쇠: `exp(-dist² / 2σ²)`, `σ = bbox 대각선 × 1.5`
- 기존 선형(`1 - dist/max`)은 사람이 조금만 걸어도 유사도가 0으로 떨어지는 문제가 있어 교체
- IoU가 0이어도 거리가 가까우면 매칭 유지 (움직이는 사람 핵심)

##### 헝가리안 알고리즘 (Kuhn-Munkres)

- 기존 greedy 매칭은 2명이 겹칠 때 잘못된 1:1 대응 발생 가능
- O(n³) 최적 매칭으로 교체, n이 보통 5~15명이라 실시간 부담 없음 (<1ms)

##### 갤러리 메커니즘

- `active_timeout_ms=3000`: 3초간 안 보이면 LOST → GALLERY로 이동
- `gallery_timeout_ms=30000`: 최대 30초간 보관
- 재등장 시 위치 유사도로 기존 stable_id 복원
- 기존 `tracking_map`의 5초 타임아웃(450000 RTP ticks) 대비 6배 긴 유지

##### EMA bbox 스무딩

- 속도에 비례하여 alpha 자동 조절 (정지: 떨림 제거, 이동: 즉시 추종)
- 점프 임계값 초과 시 alpha=0.9로 즉시 따라감

#### 7.3 적용 방식

##### camera_client.cpp

- `metadata_thread_fn`에서 `parseHumanObjectsForAnalytics()` 직후 `g_stabilizer.update()` 호출
- 결과를 `g_stable_objects`에 저장, 메인 루프에서 이를 순회하며 bbox 표시
- `on_mouse`에서 `stable_id` 기준으로 선택

##### camera_RBF.cpp

- 메인 루프에서 DeepSORT 결과(`objs`)를 가져온 직후 `g_stabilizer.update()` 호출
- `objs` 로컬 변수의 `id` 필드를 `stable_id`로 덮어씀
- 이후 코드(EMA 스무딩, 선택, PWM 계산, 원격 TRACK_POS 매칭)는 수정 없이 동작

#### 7.4 화면 표시

| 요소 | 색상 | 의미 |
|---|---|---|
| `S_001(42)` 라벨 | — | 안정 ID(카메라 원본 ID) |
| 노란색 bbox | `(0,255,255)` | 정상 추적 중 |
| 주황색 bbox | `(0,165,255)` | 가림/이탈 후 ID 복구됨 |

#### 7.5 콘솔 로그

```
✨ [NEW] 안정 ID: S_001 | 카메라 ID: 42
✨ [NEW] 안정 ID: S_002 | 카메라 ID: 55
🔗 [ID 복구] 99 → 안정 ID: S_001 (갤러리에서 복구)
```

#### 7.6 설정 (IdStabilizerConfig)

| 파라미터 | 기본값 | 설명 |
|---|---|---|
| `iou_weight` | 0.25 | IoU 유사도 가중치 |
| `distance_weight` | 0.55 | 가우시안 거리 유사도 가중치 |
| `appearance_weight` | 0.20 | 외형 특징 가중치 (없으면 자동 비활성) |
| `min_match_score` | 0.15 | 매칭 최소 점수 |
| `kalman_alpha_pos` | 0.70 | 칼만 위치 보정 게인 |
| `kalman_beta_vel` | 0.30 | 칼만 속도 학습 게인 |
| `active_timeout_ms` | 3000 | 소실 전환 시간 (ms) |
| `gallery_timeout_ms` | 30000 | 갤러리 보관 시간 (ms) |
| `bbox_ema_alpha` | 0.50 | bbox 스무딩 계수 |
| `bbox_jump_threshold` | 200.0 | 스무딩 점프 임계값 |

커스터마이징:

```cpp
static IdStabilizer g_stabilizer{[](){
    IdStabilizerConfig c;
    c.min_match_score    = 0.20f;
    c.active_timeout_ms  = 5000;
    c.gallery_timeout_ms = 60000;
    return c;
}()};
```

#### 7.7 Makefile 변경

`CAM_SRCS`와 `RBF_SRCS`에 `src/re_id.cpp` 추가:

```make
CAM_SRCS = src/camera_client.cpp \
           src/RTSPClient.cpp \
           src/XMLParser.cpp \
           src/re_id.cpp

RBF_SRCS = src/camera_RBF.cpp \
           src/RTSPClient.cpp \
           src/XMLParser.cpp \
           src/re_id.cpp
```

`app` 타겟(main.cpp)은 re_id 미사용, 변경 없음.