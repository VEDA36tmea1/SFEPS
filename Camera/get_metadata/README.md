## Camera/get_metadata 개발 정리 (2026-03-12)

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

