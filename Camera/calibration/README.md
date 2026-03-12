## `Camera/calibration` 문서 (현재까지 구현 내용)

이 폴더는 **카메라 영상(픽셀 좌표)** ↔ **실세계 평면 좌표(X,Y)** 매핑을 만들기 위한 실험/툴 모음입니다.  
주요 목표는 다음입니다.

- RTSP에서 프레임을 캡처해 샘플 이미지를 쌓기
- 내부 파라미터(`cameraParams_opencv.mat`)로 왜곡 보정(undistort) 확인
- Canny/Hough/RANSAC으로 기준 직선(ref line)들을 뽑고 JSON으로 저장
- 사용자가 찍은 점(픽셀) ↔ (X,Y) 대응점을 기반으로 Homography(= MATLAB `fitgeotrans('projective')`) 구성 및 검증

---

## 파일/스크립트 목록

### 1) 프레임 캡처

- `capture_frames.py`
  - RTSP 스트림을 `imshow`로 띄움
  - **SPACE**: 현재 프레임을 `0.png, 1.png, ...`로 저장
  - **재실행 시** 기존 `N.png` 이후 번호부터 이어서 저장
  - 종료: `q` 또는 `ESC`

실행:

```bash
cd /home/ros2man/Desktop/SFEPS/Camera/calibration
python3 capture_frames.py
```

---

### 2) 왜곡 보정(undistort) 확인

- `undistort_show.py`
  - `cameraParams_opencv.mat`의 `cameraMatrix`, `distCoeffs`로 undistort
  - `cv2.getOptimalNewCameraMatrix(..., alpha=1.0)` 사용 → **크롭 최소화(검은 여백 가능)**
  - 원본/보정 결과를 확인하는 용도

> 참고: 현재 코드는 기본으로 특정 PNG를 읽도록 되어 있으니, 필요하면 `IMG_PATH`를 바꾸거나 스크립트 인자화하면 됩니다.

---

### 3) 직선 추출/정밀 피팅 + ref 저장

- `canny_show.py`
  - 트랙바로 Canny 임계값 조절
  - HoughLinesP로 직선 후보 추출
  - 사용자가 그린 ref 선과 **각도/거리 유사한** 직선만 필터링
  - 선택된 직선 주변 edge 점으로 **RANSAC 직선 피팅**
  - (조건 만족 시) 교점 표시
  - ref 선을 JSON으로 저장

키/조작:

- **마우스 왼쪽 드래그**: ref 선 추가 (현재 최대 **5개**)
- **p**: Hough → 필터링 → RANSAC → 교점 계산 실행
- **s**: 현재 ref/canny 값 등을 `ref_lines.json`에 저장(+터미널 출력)
- **r**: 리셋
- **q / ESC**: 종료

생성 파일:

- `ref_lines.json`
  - `image`, `image_path`, `canny(low/high)`, `refs[p0,p1,angle_deg,length_px]` 등을 저장

---

### 4) 저장된 ref_lines 표시 + “휘어 보이게” 토글

- `show_ref_lines.py`
  - `ref_lines.json`을 읽어 원본 비율 유지로 표시
  - **SPACE**: `cameraParams_opencv.mat` 기반으로 “원본 위에 왜곡을 반영한 polyline(휘어 보이게)” 토글
  - 종료: `q` 또는 `ESC`

실행 예:

```bash
python3 show_ref_lines.py
python3 show_ref_lines.py ref_lines.json 74.png
```

---

### 5) (픽셀 4점 ↔ 실세계 4점) 입력 도구

- `pick_plane_points.py`
  - 이미지 위에서 **4개 점 클릭**
  - 터미널에서 각 점의 실세계 평면 좌표 **(X, Y)** 입력
  - 결과를 `plane_points.json`으로 저장

조작:

- **왼쪽 클릭**: 점 추가 (최대 4개, `P1~P4`)
- **오른쪽 클릭**: 마지막 점 undo
- 4개 찍으면 터미널로 돌아가서 엔터 → (X,Y) 입력

생성 파일:

- `plane_points.json`
  - `pixel_points`: 클릭한 픽셀 좌표
  - `world_points`: 입력한 (X,Y) 좌표 (현재 워크플로에서는 **mm 단위 권장**)

실행 예:

```bash
python3 pick_plane_points.py 75.png
```

---

### 6) Homography 매핑 뷰어 (MATLAB `fitgeotrans` 대응)

- `plane_mapping_viewer.py`
  - `plane_points.json`의 4점 대응으로 Homography \(H\) 계산
  - 창에서 픽셀을 클릭하면 **pixel → world(X,Y)** 변환 결과를 출력/오버레이
  - 종료: `q` 또는 `ESC`

실행:

```bash
python3 plane_mapping_viewer.py
```

MATLAB 대응 관계:

- `movingPoints` = `pixel_points`
- `fixedPoints` = `world_points`
- `tform = fitgeotrans(movingPoints, fixedPoints, 'projective')`
  ↔ `H, _ = cv2.findHomography(moving, fixed)`
- `transformPointsForward(tform, u, v)`
  ↔ `apply_H(H, u, v)`

---

### 7) Homography를 YAML로 내보내기 (host C++ world-track용)

- `export_homography.py`
  - `plane_points.json`을 읽어 `cv2.findHomography`로 H(3×3) 계산 후 **homography.yml** (OpenCV FileStorage) 저장
  - **rtsp_laser_demo --world-track** 에서 이 파일을 로드해, 바운딩 박스 중심 픽셀 → (X,Y) mm → pan/tilt 각도 → SET_PWM 으로 라즈베리 제어

실행:

```bash
cd Camera/calibration
python3 export_homography.py
# 또는
python3 export_homography.py plane_points.json homography.yml
```

host 쪽 실행 예 (빌드 디렉터리에서 homography.yml 경로 지정):

```bash
./rtsp_laser_demo --world-track /path/to/Camera/calibration/homography.yml [RTSP_URI]
# 레이저 위치/타겟 Z/ PWM 매핑: --laser-x 840 --laser-y -3070 --laser-z 2230 --target-z-mm 1200
```

---

### 8) (옵션) 체커보드 기반 외부파라미터 추정

- `get_extern.py`
  - 체커보드 코너를 찾고 `solvePnP`로 R,t를 뽑는 실험용 스크립트
  - `.mat` 구조 문제/환경(SciPy/NumPy) 이슈가 있어, 현재는 **실사용보단 참고/실험용**

---

## 권장 워크플로 (현재 기준)

1. `capture_frames.py`로 이미지 캡처 (`*.png`)
2. `undistort_show.py`로 왜곡 보정 상태 확인  
   - 가능하면 **undistorted 이미지로 작업**하는 쪽이 오차가 줄어듦
3. `canny_show.py`로 ref line을 그려 `ref_lines.json` 저장
4. `pick_plane_points.py`로 4점을 찍고 `plane_points.json` 저장 (mm 단위 권장)
5. `plane_mapping_viewer.py`로 Homography 기반 매핑 오차/동작 확인

---

## (추후) 장기적으로 쓸 LUT / 좌표계 방향 (추천)

Homography(4점) 기반 매핑은 빠르게 감을 잡기 좋지만, 렌즈 왜곡/클릭 오차/평면 가정 등의 영향으로 **사용 영역 밖에서 오차가 커질 수 있습니다.**  
장기적으로 “실제로 쓸” 좌표계/매핑을 만들려면, 아래처럼 **레이저를 이용해 직접 데이터를 쌓는 방식**이 더 안정적입니다.

- **아이디어**: 레이저를 그리드/코너/특정 포인트(실세계 기준점)에 실제로 맞춰가며
  - **(pixel_u, pixel_v)**: 카메라에서 탐지/측정된 레이저 픽셀 좌표
  - **(X_mm, Y_mm)**: 해당 지점의 실세계 평면 좌표(mm)
  - **(PWM_pan_us, PWM_tilt_us)**: 그 위치에서의 서보 PWM
  를 한 세트로 저장

- **장점**
  - “화면→현실” 변환을 카메라 모델에만 의존하지 않고, **실제 시스템(레이저+서보+탐지)** 기준으로 보정
  - AutoLUT/그리드 수집 플로우와 결이 같아 확장/자동화가 쉬움

- **권장 데이터 포맷 예시(개념)**
  - `lut_world_pixel_pwm.json` 같은 파일에 grid/point 별로:
    - `pixel: {u,v}`, `world: {X_mm,Y_mm}`, `pwm: {pan_us,tilt_us}`
    - 필요하면 `timestamp`, `frame_id`, `confidence` 등도 같이 기록

- **활용**
  - (pixel_u, pixel_v) → (X_mm, Y_mm) 매핑을 LUT + 2D 보간으로 만들거나
  - (X_mm, Y_mm) → PWM을 직접 보간(이미 구현한 LUT 기반 추적과 동일한 방향)

---

## 주의/팁

- 큰 PNG(수천×수천)는 GUI에서 렉/프로세스 `Killed`가 날 수 있어, 필요하면 **리사이즈한 이미지로 작업**하는 게 안전합니다.
- 4점 Homography는 영역 밖에서 오차가 커질 수 있습니다.  
  (정확도를 더 올리려면 “더 많은 점” + “RANSAC/최소자승”으로 확장하는 방향이 유리합니다.)

