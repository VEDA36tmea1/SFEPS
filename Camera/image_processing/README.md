# Camera ISP Pipeline

libcamera 기반 RAW 이미지 취득부터 ISP 처리, 화질 최적화까지 수행하는 임베디드 카메라 캡처 시스템입니다.  
Unix Domain Socket(UDS)을 통해 외부 서버로부터 캡처 요청을 수신하고, 처리된 이미지를 지정 경로에 저장합니다.

---

## 파일 구성

```
├── camera_client.cpp   # 메인 진입점 — 스레드 관리, UDS 트리거 수신, 캡처 파이프라인 조율
├── img_processing.cpp  # ISP 및 후처리 구현체
└── inc/
    └── img_processing.h  # 공개 인터페이스 선언
```

---

## 전체 처리 흐름

```
[libcamera GStreamer Pipeline]
        │
        ▼
  captureThreadFunc          ← 최신 프레임을 g_raw_queue에 지속 적재
        │
  triggerListenerThread      ← UDS로 CAPTURE_REQ 수신 → g_request_queue에 적재
        │
        ▼
  pipelineWorkerThread
        │
        ├─ [RAW 모드]  runPureISP()         → CV_16UC1 → CV_8UC3 BGR
        │               BLC → AWB/AE → Demosaic → CCM → Gamma
        │
        └─ [BGR 모드]  libcamera BGR 직접 수신
                │
                ▼
        processISPAndGetBest()
                ShadowBoost × 7 + CLAHE × 7 → 엔트로피 평가 → Best 선택
                │
                ▼
        JPEG 저장 → /home/iam/SFEPS/event_images/pending/
```

---

## 주요 컴포넌트

### `camera_client.cpp`

#### 스레드 구성

| 스레드 | 역할 |
|--------|------|
| `captureThreadFunc` | GStreamer 파이프라인에서 프레임을 지속 획득, `g_raw_queue`에 보관 |
| `triggerListenerThread` | UDS 소켓(`/tmp/sfeps_camera_trigger.sock`)에서 `CAPTURE_REQ` 수신 |
| `pipelineWorkerThread` | 큐에서 요청을 꺼내 ISP 파이프라인 실행 후 이미지 저장 |
| `signalWaitThread` | `SIGINT` / `SIGTERM` 수신 시 graceful shutdown 수행 |

#### 트리거 프로토콜

UDS 소켓에 아래 형식의 한 줄 문자열을 전송하면 캡처가 실행됩니다.

```
CAPTURE_REQ|REQ_ID=<id>|OBJECT_ID=<obj>|TAG=<tag>|OUT=<output_path>\n
```

| 필드 | 설명 |
|------|------|
| `REQ_ID` | 요청 식별자 |
| `OBJECT_ID` | 촬영 대상 객체 ID |
| `TAG` | 이벤트 태그 |
| `OUT` | 저장 경로 (반드시 `/home/iam/SFEPS/event_images/pending/` 하위) |

> 출력 경로는 경로 순회(`..`) 및 개행 문자를 포함할 수 없습니다.

#### GStreamer 파이프라인

시작 시 RAW 모드를 우선 시도하고, 실패 시 BGR 모드로 폴백합니다.

```
# RAW 모드 (SRGGB10, 10-bit Bayer)
libcamerasrc ! video/x-raw,format=SRGGB10,width=1920,height=1080,framerate=30/1 ! appsink ...

# BGR 폴백
libcamerasrc ! video/x-raw,width=1920,height=1080,framerate=30/1 ! videoconvert ! video/x-raw,format=BGR ! appsink ...
```

노출 관련 기본값은 다음과 같습니다. 역광 환경에서는 `kExposureTimeUs`를 낮춰 포화를 억제합니다.

```cpp
constexpr int   kExposureTimeUs = 8000;  // 8 ms
constexpr float kAnalogueGain   = 1.0f;
```

---

### `img_processing.cpp` / `img_processing.h`

#### `runPureISP(const cv::Mat& raw16_frame)` — RAW ISP 경로

CV_16UC1 Bayer 프레임을 CV_8UC3 BGR 이미지로 변환합니다.

| 단계 | 처리 내용 |
|------|-----------|
| **BLC** | 블랙 레벨(`black_level=64`) 감산 |
| **AWB** | Gray World 방식으로 R/B 채널 게인 계산 |
| **AE** | 평균 밝기 기반 자동 노출 게인 적용 (0.5 ~ 3.0 범위 클램프) |
| **Highlight Rolloff** | 픽셀값 950 이상 구간에서 채널 게인을 G 게인으로 블렌딩하여 색 왜곡 억제 |
| **Demosaic** | SBGGR 패턴 기준 Bilinear Interpolation |
| **CCM** | 3×3 색 보정 행렬 적용 (하이라이트 영역 블렌딩 포함) |
| **Gamma** | LUT 방식 γ=2.2 보정 |

#### `processISPAndGetBest(const cv::Mat& frame_in, cv::Mat& tuning_view_out)` — 후처리 경로

CV_8UC3 BGR 이미지를 입력받아 최적 화질 후보를 선택합니다.

1. **8개 후보 생성**: 원본 1장 + ShadowBoost/CLAHE 파라미터 조합 7가지
2. **엔트로피 평가**: 각 후보의 이미지 엔트로피를 계산하여 가장 높은 값을 선택
3. **튜닝 뷰 생성**: 4×2 그리드 비교 이미지를 `tuning_view_out`에 출력
4. **히스토그램 저장**: 보정 전/후 픽셀 분포 비교 이미지를 `pixel_distribution_comparison.jpg`로 저장

ShadowBoost 파라미터 조합 (Gamma, Alpha):

| 후보 | Gamma | Alpha | CLAHE Clip |
|------|-------|-------|------------|
| Lv.1 | 1.2 | 1.0 | 1.5 |
| Lv.2 | 1.5 | 1.2 | 2.0 |
| Lv.3 | 1.8 | 1.4 | 2.5 |
| Lv.4 | 2.2 | 1.6 | 3.0 |
| Lv.5 | 2.5 | 1.8 | 3.5 |
| Lv.6 | 2.8 | 2.0 | 4.0 |
| Lv.7 | 3.0 | 2.2 | 4.5 |

#### 단독 사용 가능한 함수

```cpp
// YUV Y채널 기반 그림자 영역 감마/톤 보정
void applyShadowBoost(const cv::Mat& src, cv::Mat& dst, double gamma, double alpha);

// CLAHE (Contrast Limited Adaptive Histogram Equalization)
void applyCLAHE(const cv::Mat& src, cv::Mat& dst, double clip_limit, cv::Size grid);
```

---

## 빌드 요구사항

- **OS**: Linux (UDS 소켓 사용)
- **컴파일러**: C++17 이상
- **의존성**:
  - OpenCV 4.x (`opencv2/opencv.hpp`, `imgproc`, `dnn`)
  - GStreamer (libcamera GStreamer 플러그인 포함)
  - libcamera
  - pthread

---

## 동작 모드

`camera_client.cpp` 상단의 매크로로 모드를 전환합니다.

```cpp
#define LIVE_CAMERA_MODE 1   // 1: 실제 카메라, 0: 로컬 이미지 테스트
```

테스트 모드(`0`)에서는 `img/test_image.jpg`를 입력으로 사용하고, 결과를 `4_best_shot_local.jpg`로 저장합니다.

---

## 주요 상수 / 설정값

| 상수 | 기본값 | 설명 |
|------|--------|------|
| `kTriggerSocketPath` | `/tmp/sfeps_camera_trigger.sock` | UDS 소켓 경로 |
| `kPendingBaseDir` | `/home/iam/SFEPS/event_images/pending` | 이미지 저장 기본 디렉토리 |
| `kRequestQueueMax` | `5` | 최대 대기 요청 수 |
| `kClientReadTimeoutMs` | `1000` | 클라이언트 읽기 타임아웃 (ms) |
| `kExposureTimeUs` | `8000` | 셔터 속도 (µs) |
| `ISPConfig::black_level` | `64` | 센서 블랙 레벨 (10-bit 기준) |

---

## Shutdown 동작

`SIGINT` 또는 `SIGTERM` 수신 시:

1. 대기 중인 모든 캡처 요청 폐기
2. 최신 프레임 버퍼 초기화
3. UDS 소켓 및 클라이언트 fd 닫기
4. 소켓 파일(`/tmp/sfeps_camera_trigger.sock`) 삭제
5. 카메라 파이프라인 해제
6. 전체 경과 시간 로그 출력
