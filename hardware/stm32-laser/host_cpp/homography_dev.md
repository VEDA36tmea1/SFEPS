## Homography 기반 World-Track 개발 정리

### 목표

기존 `rtsp_laser_demo`는 “픽셀 오차(EX/EY)” 또는 “픽셀 타겟(TU/TV)”을 라즈베리로 보내 PID/LUT로 PWM을 제어하는 구조였다.  
여기에 **픽셀 좌표를 실세계 평면 좌표(mm)로 변환(Homography)** 한 뒤, **레이저 원점과 타겟 높이(Z)를 가정하여 3D 방향 벡터를 만들고 pan/tilt 각도를 계산**해 **`SET_PWM`** 으로 라즈베리 서보를 직접 구동하는 “world-track” 흐름을 추가했다.

---

### 핵심 아이디어

- 카메라 영상의 **바운딩 박스 중심 픽셀 (u,v)** 를 입력으로 사용
- Homography \(H\) 를 이용해 **Z=0 평면에서의 (X,Y) mm** 로 변환
- 타겟의 높이를 \(Z = Z_{target}\) (예: 사람 가슴 높이 1200mm)로 가정
- 레이저(서보) 원점 위치 \((X_L, Y_L, Z_L)\) 를 알고 있다고 가정
- 3D 벡터를 구성해서 pan/tilt 각도를 계산
- 각도를 PWM(µs)으로 선형 매핑하여 `SET_PWM,PAN=...,TILT=...` 를 stdout으로 출력  
  → `ubuntu_tcp_server`가 이를 그대로 라즈베리로 전달  
  → 라즈베리 `pid_pwm_agent.py` 는 `SET_PWM`을 받으면 PID/LUT 우회하여 PWM을 즉시 적용

---

### 변경된 주요 파일

- **Host C++**
  - `src/rtsp_laser_demo.cpp`
    - `--world-track` 모드 추가
    - `homography.yml` 로드(`cv::FileStorage`, key=`H`)
    - 바운딩 박스 중심 픽셀 → (X,Y) 변환 → (X,Y,Z) → pan/tilt deg → PWM µs → `SET_PWM` 전송

- **Calibration (Python)**
  - `Camera/calibration/plane_points.json`
    - 픽셀 4점 ↔ 월드 4점(mm) 대응
  - `Camera/calibration/export_homography.py`
    - `plane_points.json` → `cv2.findHomography` → `homography.yml` 생성 (OpenCV FileStorage)

---

### 좌표계/수식

#### 1) 픽셀 → 월드 평면(mm)

Homography \(H\) 가 “pixel → world(mm)” 변환이라고 하면:

\[
\begin{bmatrix}
X \\\\
Y \\\\
1
\end{bmatrix}
\propto
H
\begin{bmatrix}
u \\\\
v \\\\
1
\end{bmatrix}
\]

구현에서는 \(w\) 로 나눈 정규화 형태:

- \(w = h_{20}u + h_{21}v + h_{22}\)
- \(X = (h_{00}u + h_{01}v + h_{02})/w\)
- \(Y = (h_{10}u + h_{11}v + h_{12})/w\)

#### 2) 월드 3D 점 → pan/tilt 각도

- 레이저 원점: \((X_L, Y_L, Z_L)\)
- 타겟: \((X, Y, Z_{target})\)
- 차이 벡터:
  - \(dx = X - X_L\)
  - \(dy = Y - Y_L\)
  - \(dz = Z_{target} - Z_L\)
- 수평 거리: \(r = \sqrt{dx^2 + dy^2}\)

각도(도):

- \(pan = \mathrm{atan2}(dx, dy)\cdot 180/\pi\)
- \(tilt = \mathrm{atan2}(dz, r)\cdot 180/\pi\)

> pan/tilt 부호는 프로젝트의 축 정의에 따라 바뀔 수 있다. 현재 구현은 “Y=전방, X=우측”을 기준으로 `atan2(dx,dy)`를 사용한다.

#### 3) 각도 → PWM(µs) 선형 매핑

- `pan_us  = pan_center_us  + pan_deg  * pan_us_per_deg`
- `tilt_us = tilt_center_us + tilt_deg * tilt_us_per_deg`
- 범위 클램프: 800~2200us

`pan_us_per_deg`, `tilt_us_per_deg` 는 실제 서보/기구에 맞춰 캘리브가 필요하다(초기값은 임시).

---

### `rtsp_laser_demo` 실행 방법

#### 1) Homography 생성

`Camera/calibration`에서 `plane_points.json`을 준비(픽셀 4점 ↔ 월드 4점(mm)).

```bash
cd /home/ros2man/Desktop/SFEPS/Camera/calibration
python3 export_homography.py
```

기본 출력: `homography.yml`

#### 2) Host 실행 (world-track)

빌드 폴더에서:

```bash
cd /home/ros2man/Desktop/SFEPS/hardware/stm32-laser/host_cpp/build
./rtsp_laser_demo --world-track /home/ros2man/Desktop/SFEPS/Camera/calibration/homography.yml "rtsp://..."
```

주요 옵션:

- `--world-track [homography.yml]`: world-track 모드 ON (파일 경로 생략 시 기본 `homography.yml`)
- `--homography <path>`: homography 경로 지정(보조 옵션)
- `--laser-x <mm>` `--laser-y <mm>` `--laser-z <mm>`: 레이저 원점 (기본 840, -3070, 2230)
- `--target-z-mm <mm>`: 타겟 높이(기본 1200)
- `--pan-center-us <us>` `--tilt-center-us <us>`: 각도 0일 때 PWM 중심
- `--pan-us-per-deg <us/deg>` `--tilt-us-per-deg <us/deg>`: 각도→PWM 기울기

동작 조건(현재 구현):

- `client_connected == true`(라즈베리 연결 FIFO가 CONNECTED를 받음)
- `targetROI.valid == true` (바운딩 박스가 있어야 함)
- 주기: `SEND_EVERY_N` 프레임마다 `SET_PWM` 출력

---

### 데이터 흐름 (프로세스 파이프)

```
rtsp_laser_demo (host)
  - bbox center (u,v)
  - world (X,Y) via Homography
  - pan/tilt 계산
  - stdout: SET_PWM,PAN=...,TILT=...
      |
      v
ubuntu_tcp_server
  - SET_PWM 라인을 그대로 Raspi로 전달
      |
      v
pid_pwm_agent.py (Raspi)
  - SET_PWM 수신 시: PID/LUT 우회, PWM 즉시 적용
```

---

### 한계/주의사항

- Homography는 “하나의 평면(Z=0)”에서만 유효하다. 사람의 실제 몸은 3D이고, 여기서는 `target_z_mm`로 높이를 고정 가정한다.
- `pan_us_per_deg`, `tilt_us_per_deg` 선형 모델은 기구/서보에 따라 오차가 크다. 실제 측정으로 보정 필요.
- Homography의 정확도는 `plane_points.json`의 4점 입력 품질에 크게 의존한다. (가능하면 더 많은 점 + RANSAC/최소자승으로 확장하는 방향 추천)

