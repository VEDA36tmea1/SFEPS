## IBVS Host C++ / ibvs_core 라이브러리

이 디렉터리는 `IMPLEMENTATION_PLAN_IBVS.md` 기반으로 작성한
**IBVS 호스트(C++) 스켈레톤 코드 + 공용 라이브러리(`ibvs_core`)** 를 포함한다.

- 비전: `VisionDetector` (단순 스텁 – 실제 프로젝트에서 교체 필요)
- 제어: `IbvsController` (P 제어)
- 통신: `StmInterface` (리눅스 termios 기반 UART)
- GStreamer 연동: `process_gst_frame` (`gst_ibvs.cpp`)
- 데모 메인 루프: `main_ibvs.cpp` (`ibvs_host` 실행 파일)

### 구조

- `CMakeLists.txt`
- `include/vision_detector.h`
- `include/ibvs_controller.h`
- `include/stm_interface.h`
- `include/gst_ibvs.h`
- `src/vision_detector.cpp`
- `src/ibvs_controller.cpp`
- `src/stm_interface.cpp`
- `src/gst_ibvs.cpp`
- `src/main_ibvs.cpp`
- `src/rtsp_laser_demo.cpp`

### 빌드 방법 (예시)

```bash
cd hardware/stm32-laser/host_cpp
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
```

OpenCV와 GStreamer가 패키지로 설치되어 있고
`find_package(OpenCV)`, `pkg_check_modules(GST ...)` 가 동작해야 한다.
필요하면 `OpenCV_DIR`, `PKG_CONFIG_PATH` 등을 CMake 옵션/환경변수로 넘긴다.

### Ninja 빌드 명령어 (복붙용)

Ninja는 단독으로 configure를 하지 못하므로, 처음 1회는 CMake로 `build.ninja`를 생성해야 한다.

#### 처음 1회 (configure + build)

```bash
cmake -S hardware/stm32-laser/host_cpp -B hardware/stm32-laser/host_cpp/build-ninja -G Ninja
ninja -C hardware/stm32-laser/host_cpp/build-ninja
```

#### 이후 빌드만 다시

```bash
ninja -C hardware/stm32-laser/host_cpp/build-ninja
```

#### 클린 빌드

```bash
ninja -C hardware/stm32-laser/host_cpp/build-ninja -t clean
ninja -C hardware/stm32-laser/host_cpp/build-ninja
```

#### 실행

```bash
./hardware/stm32-laser/host_cpp/build-ninja/ibvs_host /dev/ttyUSB0 0
```

### 실행 예시

```bash
./ibvs_host /dev/ttyUSB0 0
```

- 첫 번째 인자: STM 보드가 연결된 시리얼 포트 (기본: `/dev/ttyUSB0`)
- 두 번째 인자: 카메라 인덱스 (기본: `0`)

현재 비전·검출 로직은 **테스트용 스텁**이다.
실제 타겟/레이저 검출 알고리즘으로 교체해 사용해야 한다.

---

### RTSP 레이저 검출 데모 (`rtsp_laser_demo`)

카메라를 **RTSP 스트림**으로 받아서 `VisionDetector` 로 레이저를 찾는 간단한 데모 실행 파일.
**타겟 ROI**: 마우스 클릭 후 드래그하면 고정 크기(120×120) 바운딩 박스가 마우스를 따라가며,
향후 사람 검출로 전환 시 `ITargetProvider` 구현체만 교체하면 된다. (설계: `docs/TARGET_ROI_DESIGN.md`)

#### 1. 빌드

일반 빌드와 동일하게 `host_cpp` 전체를 빌드하면 함께 생성된다.

```bash
cd hardware/stm32-laser/host_cpp
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j
```

#### 2. 실행

기본 RTSP URI (하드코딩 값):

```bash
./build/rtsp_laser_demo
```

또는 다른 RTSP 주소를 인자로 넘길 수 있다:

```bash
./build/rtsp_laser_demo rtsp://admin:CCgbdCCgbd@192.168.0.11/profile2/media.smp
```

동작:

- OpenCV `cv::VideoCapture` 로 RTSP 스트림을 열고,
- **마우스 클릭 후 드래그**로 타겟 바운딩 박스를 지정(가변 크기). 박스 안을 드래그하면 박스가 이동.
- 각 프레임마다 `VisionDetector::detectLaser(frame)` 을 호출해서 레이저 스폿을 찾는다.
- 레이저를 찾으면:
  - `stderr` 로 좌표 로그 출력:

    ```text
    [rtsp_laser_demo] frame 123 laser=(x, y)
    ```

  - 영상 위에 빨간 점으로 시각화 후 `imshow("rtsp_laser_demo", frame)` 윈도우에 표시.
- `ESC` 또는 `q` 키를 누르면 종료.

#### 3. 파이프라인 기반 레이저 트래킹 (`rtsp_laser_demo | ubuntu_tcp_server`)

`rtsp_laser_demo` 에 **IBVS P 제어기(`IbvsController`)** 를 붙여,
타겟 박스 중심과 레이저 위치 사이 픽셀 오차를 기반으로 **PWM(us)** 를 계산하고,
표준 출력(`stdout`)으로 `PAN_US TILT_US` 형식의 한 줄(`"1500 1400\n"`)을 출력하도록 구성했다.

이를 `tmp_server/ubuntu_server/ubuntu_tcp_server` 와 파이프로 연결하면:

```bash
# 터미널 1: STM32 + ESP + Wi-Fi AP(10.42.0.1 등) 준비
# 터미널 2: Ubuntu TCP 서버 실행
cd hardware/stm32-laser/tmp_server/ubuntu_server
make ubuntu_tcp_server
./ubuntu_tcp_server          # stdin 에서 "us_x us_y" 를 읽어 ESP로 전송

# 터미널 3: 카메라 RTSP → 레이저 트래킹 → PWM(us) 생성 → 파이프로 서버에 전달
cd hardware/stm32-laser/host_cpp/build
./rtsp_laser_demo | ../../tmp_server/ubuntu_server/ubuntu_tcp_server
```

파이프라인 전체 흐름:

```text
카메라/마우스           rtsp_laser_demo(stdout)         ubuntu_tcp_server(stdin)    ESP/TCP          STM32(UART1)        Servo
--------------    -------------------------------    -----------------------------   ------------   -------------------   ---------------
RTSP 프레임 ───▶  타겟ROI(center), 레이저검출 ───▶  "PAN_US TILT_US\n" ───────▶   "1500 1400"  ─▶  "+IPD,...1500 1400" ─▶  Servo_SetAllUs
```

- `rtsp_laser_demo`:
  - 마우스로 만든 박스 중심(`TargetROI.center()`)과 레이저 위치 사이 픽셀 오차를 계산.
  - `IbvsController(800, 2200, 1500, 0.5, 0.5)` 로 **단순 P 제어** 수행  
    (현재 구현은 **P 제어만 있으며, I/D 항은 없음**).
  - 결과 PWM(us)을 `stdout` 으로 `"pan_us tilt_us\n"` 형식으로 출력 (로그는 `stderr`).
- `ubuntu_tcp_server`:
  - `stdin` 에서 한 줄씩 `"1500 1400"` 을 읽어 ESP/STM32로 그대로 전송.
- STM32 (`main.c`):
  - WiFi(USART1)에서 들어온 `"1500 1400"` 을 UART2 명령 파서와 동일 형식으로 처리하도록  
    `wifi_line` 파서에 서보 명령 해석을 추가하면, `Servo_SetAllUs(pan_us, tilt_us)` 로 PA8/PA0 PWM 제어 가능.

#### 타겟 ROI 설계 (`docs/TARGET_ROI_DESIGN.md`)

- `ITargetProvider` 인터페이스로 타겟 ROI 제공.
- 현재: `MouseDragTargetProvider` (마우스 드래그).
- 향후: `PersonDetectorTargetProvider` (YOLO 등)로 교체 시 Provider만 교체하면 됨.

### GStreamer 파이프라인 사용 가이드

`ibvs_core` 라이브러리는 GStreamer 파이프라인 안에서 사용할 수 있도록
`process_gst_frame(GstSample*, cv::Point2f, ...)` 헬퍼 함수를 제공한다.

#### 1. 개념 흐름

1. GStreamer 파이프라인에서 카메라/스트림 영상을 `appsink` 로 받는다.
2. 다른 요소(딥러닝 detector 등)가 객체 중심 좌표 `target_center` 를 계산한다.
3. `appsink` 의 `new-sample` 콜백에서:
   - `GstSample* sample` 을 받는다.
   - `process_gst_frame(sample, target_center, detector, controller, stm);` 호출
4. 함수 내부에서:
   - `GstSample` → `cv::Mat` 변환
   - 영상에서 레이저 스폿 좌표 검출
   - `target_center` 와 레이저 좌표 사이 픽셀 오차 계산
   - `IbvsController` 로 PWM(us) 계산
   - `StmInterface` 로 STM 보드에 `"PAN_US TILT_US\r\n"` 전송

#### 2. appsink 파이프라인 예시 (gst-launch 스타일)

아래는 개념적인 예시이며, 실제 앱에서는 C/C++ 코드로 파이프라인을 구성하고
`appsink` 의 `new-sample` 시그널에 콜백을 연결해야 한다.

```bash
gst-launch-1.0 \
  v4l2src device=/dev/video0 ! \
  videoconvert ! video/x-raw,format=BGR,width=640,height=480 ! \
  queue ! \
  appsink name=mysink emit-signals=true sync=false
```

위에서 `mysink` 의 `new-sample` 콜백에서 `process_gst_frame` 을 호출한다.

#### 3. C++ appsink 콜백 예시 (개념 코드)

```cpp
// 전역 또는 컨텍스트에 유지할 객체들
VisionDetector   g_detector;
IbvsController   g_controller(800, 2200, 1500, 0.5, 0.5);
StmInterface     g_stm("/dev/ttyUSB0", 115200);
cv::Point2f      g_target_center; // 외부 객체 검출 모듈이 갱신

static GstFlowReturn on_new_sample(GstElement* sink, gpointer)
{
    GstSample* sample = nullptr;
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (!sample)
        return GST_FLOW_ERROR;

    // g_target_center 는 다른 모듈에서 이미 계산해둔 객체 중심 좌표
    process_gst_frame(sample, g_target_center,
                      g_detector, g_controller, g_stm);

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}
```

GStreamer 파이프라인 구성 시 `appsink` 에 위 콜백을 연결하면,
각 프레임마다 레이저 검출 + 에러 보정 + STM 명령 전송이 수행된다.

---

### src 코드별 데이터 흐름도

#### 1. `main_ibvs.cpp` (데모 실행 파일)

텍스트 데이터 흐름:

1. 프로그램 시작
2. 시리얼 포트 오픈 → `StmInterface` 생성 → 필요 시 `sendModeManual()`
3. `cv::VideoCapture` 로 카메라 프레임 획득
4. 루프:
   - `frame` 캡처 (`cap.read`)
   - `VisionDetector::detectTarget(frame)` → `target`
   - `VisionDetector::detectLaser(frame)`  → `laser`
   - 둘 다 `found == true` 이면:
     - `e_u = target.x - laser.x`, `e_v = target.y - laser.y`
     - `IbvsController::update(e_u, e_v, dt)` → `pan_us`, `tilt_us`
     - `StmInterface::sendPwm(pan_us, tilt_us)` 으로 STM에 전송
   - 디버그용으로 화면에 타겟/레이저/선 오버레이 후 `imshow`

간단한 흐름도(논리):

`카메라 프레임 → VisionDetector → (target, laser)`
`→ 에러 계산(e_u, e_v) → IbvsController → PWM(us)`
`→ StmInterface → UART → STM 보드`

#### 2. `gst_ibvs.cpp` (`process_gst_frame`)

텍스트 데이터 흐름:

1. GStreamer `appsink` 에서 `GstSample* sample` 을 받는다.
2. `sampleToMat(sample, frame)`:
   - `GstSample` 의 캡스에서 `width`, `height`, `format` 읽기
   - `GstBuffer` 매핑 → `cv::Mat` 생성 → 복사본으로 `frame` 완성
3. 외부에서 전달된 `target_center` 를 사용해 `DetectionResult target` 구성
4. `VisionDetector::detectLaser(frame)` → 레이저 중심 `laser`
5. `e_u = target.x - laser.x`, `e_v = target.y - laser.y`
6. `IbvsController::update(e_u, e_v, 0.0)` → `pan_us`, `tilt_us`
7. `StmInterface::sendPwm(pan_us, tilt_us)` 로 STM에 명령 전송

논리 흐름도:

`GstSample → sampleToMat → cv::Mat frame`
`frame → VisionDetector::detectLaser → laser_center`
`(target_center, laser_center) → 에러(e_u, e_v)`
`→ IbvsController → PWM(us) → StmInterface → STM`

#### 3. `vision_detector.cpp` (`VisionDetector`)

- `detectTarget(const cv::Mat&)`
  - 현재는 **프레임 중앙을 타겟으로 가정**하는 스텁 구현.
  - 실제 프로젝트에서는 객체 검출 결과(bbox 중심 등)로 교체해야 한다.

- `detectLaser(const cv::Mat&)`
  - 입력 `frame` 을 GRAY로 변환 (`cvtColor` 또는 채널 수 1이면 그대로 사용).
  - `cv::minMaxLoc` 으로 이미지 전체에서 **최대 밝기 픽셀**의 위치(`maxLoc`)와 값(`maxVal`)을 찾음.
  - 최대 밝기 픽셀을 **레이저 포인터 스폿의 중심**이라고 가정:

    ```cpp
    result.point = cv::Point2f(maxLoc.x, maxLoc.y);
    ```

  - `maxVal > 50.0` (임계값, 경험적으로 조정) 인 경우에만 `found = true` 로 간주.

  - 이 구현은 매우 단순한 스텁으로:
    - 프레임 내에서 레이저 포인터가 **가장 밝은 점**이라는 가정을 둔다.
    - 주변 조명이 강하거나 반사가 많으면 오검출될 수 있으므로,
      실제 환경에서는 색/HSV threshold, 블러/열림 연산, contour 필터 등으로 보강해야 한다.

흐름:

`cv::Mat frame → (색/밝기 기반 처리) → DetectionResult {point, found}`

#### 4. `ibvs_controller.cpp` (`IbvsController`)

- 내부 상태:
  - `current_pan_us`, `current_tilt_us`
  - PWM 제한값 `pwm_min_`, `pwm_max_`, 중립값 `neutral_`
  - 게인 `Ku_`, `Kv_` (각각 [us / pixel])

- `update(e_u, e_v, dt_sec)`:
  - 단순 P 제어:
    - `current_pan_us  += Ku_ * e_u`
    - `current_tilt_us += Kv_ * e_v`
  - `clampPwm` 으로 `PWM_US_MIN` ~ `PWM_US_MAX` 범위 제한
  - `IbvsOutput { pan_us, tilt_us }` 반환

흐름:

`(e_u, e_v) → P 제어 → (pan_us, tilt_us)`

#### 5. `stm_interface.cpp` (`StmInterface`)

- 생성자:
  - `openSerial(device, baudrate)` 로 리눅스 시리얼 포트 오픈 (`termios` 설정 포함)
- `isOpen()`:
  - 파일 디스크립터 유효 여부 반환
- `sendPwm(pan_us, tilt_us)`:
  - `"PAN_US TILT_US\r\n"` 형식 문자열 구성 후 `write` 로 전송
- `sendModeManual()`:
  - `"mode 0\r\n"` 문자열 전송 (STM 펌웨어의 manual 모드 진입용)

흐름:

`(pan_us, tilt_us) → 문자열 "u1 u2\r\n" → UART → STM 보드`

