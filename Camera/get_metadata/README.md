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

