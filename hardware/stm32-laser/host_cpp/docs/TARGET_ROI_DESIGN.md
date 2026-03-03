# 타겟 ROI(바운딩 박스) 설계 문서

## 1. 개요

레이저 추적 시스템에서 **타겟(추적 대상)** 은 화면 상의 관심 영역(ROI)으로 표현된다.
현재는 마우스 드래그로 ROI를 지정하고, 이후 **실제 사람 객체 검출**로 전환할 수 있도록
인터페이스를 분리하여 설계한다.

## 2. 요구사항

| 항목 | 설명 |
|------|------|
| ROI 형식 | 일정 크기의 직사각형 바운딩 박스 |
| 지정 방식 | 마우스 클릭 후 드래그 시 박스 중심이 마우스 위치를 따라감 |
| 확장성 | 나중에 사람 검출(YOLO, OpenCV DNN 등) 결과로 ROI를 대체 가능 |

## 3. 아키텍처

```
┌─────────────────────────────────────────────────────────────┐
│                    rtsp_laser_demo (메인 루프)                │
│  - VideoCapture → frame                                      │
│  - ITargetProvider::getTarget(frame) → TargetROI             │
│  - VisionDetector::detectLaser(frame) → laser point          │
│  - ROI 박스 그리기, 레이저 점 그리기, imshow                  │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    ITargetProvider (인터페이스)               │
│  TargetROI getTarget(const cv::Mat& frame)                   │
└─────────────────────────────────────────────────────────────┘
         ▲                                    ▲
         │                                    │
┌────────┴────────┐                ┌─────────┴─────────┐
│ MouseDragTarget │                │ PersonDetector    │
│ Provider        │                │ TargetProvider     │
│ (현재 구현)      │                │ (향후 구현)         │
└─────────────────┘                └───────────────────┘
```

## 4. 데이터 구조

### 4.1 TargetROI

```cpp
struct TargetROI {
    cv::Rect rect;   // 바운딩 박스 (x, y, width, height)
    bool valid;      // 유효한 ROI가 있는지
    cv::Point2f center() const;  // rect의 중심 좌표
};
```

- `rect`: OpenCV `cv::Rect` 형식. 프레임 내부로 클램핑됨.
- `valid`: `true`이면 ROI가 설정되어 있음. `false`이면 아직 사용자가 지정하지 않음.
- `center()`: IBVS 제어에서 타겟 중심으로 사용.

## 5. ITargetProvider 인터페이스

```cpp
class ITargetProvider {
public:
    virtual ~ITargetProvider() = default;
    virtual TargetROI getTarget(const cv::Mat& frame) = 0;
};
```

- `getTarget(frame)`: 현재 프레임 기준으로 타겟 ROI를 반환.
- 프레임 크기로 ROI를 클램핑하는 것은 각 구현체에서 처리.

## 6. MouseDragTargetProvider (현재 구현)

### 6.1 동작

1. **초기 상태**: `valid = false`. 박스가 화면에 표시되지 않음.
2. **첫 클릭**: 마우스 왼쪽 버튼 다운 → 박스 중심을 클릭 위치로 설정, `valid = true`.
3. **드래그**: 버튼을 누른 채 이동 → 박스 중심이 마우스 위치를 따라감.
4. **클릭 해제**: 버튼 업 → 박스 위치 고정.

### 6.2 파라미터

| 파라미터 | 기본값 | 설명 |
|----------|--------|------|
| boxWidth | 120 | 바운딩 박스 가로 크기 (픽셀) |
| boxHeight | 120 | 바운딩 박스 세로 크기 (픽셀) |

### 6.3 마우스 콜백

- `EVENT_LBUTTONDOWN`: 드래그 시작, 중심 = (x, y)
- `EVENT_MOUSEMOVE` + `FLAG_LBUTTON`: 드래그 중, 중심 = (x, y)
- `EVENT_LBUTTONUP`: 드래그 종료

### 6.4 프레임 경계 처리

- 박스 중심이 프레임 밖으로 나가지 않도록 `rect`를 `cv::Rect(0,0,cols,rows)` 내부로 클램핑.

## 7. PersonDetectorTargetProvider (향후 구현)

### 7.1 개요

- OpenCV DNN, YOLO, TensorRT 등으로 사람 검출.
- 검출된 첫 번째 사람 bbox를 `TargetROI`로 반환.
- 검출 실패 시 `valid = false` 또는 이전 프레임 ROI 유지(구현 선택).

### 7.2 전환 방법

```cpp
// 현재: 마우스 드래그
std::unique_ptr<ITargetProvider> provider =
    std::make_unique<MouseDragTargetProvider>(120, 120);

// 향후: 사람 검출
// std::unique_ptr<ITargetProvider> provider =
//     std::make_unique<PersonDetectorTargetProvider>("yolov8n.onnx");
```

메인 루프는 `provider->getTarget(frame)` 만 호출하므로, Provider 교체만으로 전환 가능.

## 8. rtsp_laser_demo 연동

1. `MouseDragTargetProvider` 생성 및 `attachToWindow("rtsp_laser_demo")` 호출.
2. 매 프레임:
   - `TargetROI roi = provider->getTarget(frame)`
   - `roi.valid`이면 `cv::rectangle(frame, roi.rect, ...)` 로 박스 그리기.
   - 레이저 검출 결과와 함께 표시.
3. (선택) 터미널에 ROI 중심 또는 rect 출력.

## 9. 파일 구성

| 파일 | 역할 |
|------|------|
| `include/target_provider.h` | `TargetROI`, `ITargetProvider`, `MouseDragTargetProvider` 선언 |
| `src/mouse_target_provider.cpp` | `MouseDragTargetProvider` 구현 |
| `docs/TARGET_ROI_DESIGN.md` | 본 설계 문서 |

## 10. 사람 검출 전환 시 체크리스트

- [ ] `PersonDetectorTargetProvider` 클래스 추가
- [ ] 모델 파일 경로/로딩 (ONNX, pb 등)
- [ ] `getTarget()` 내부에서 inference → bbox → `TargetROI` 변환
- [ ] CMakeLists에 OpenCV DNN 또는 ONNX Runtime 링크
- [ ] `main`에서 Provider 인스턴스만 교체
