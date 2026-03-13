# PID → LUT 기반 트래킹 모드 정리

이 문서는 현재 레이저 트래킹 시스템에서

- **PID 기반 IBVS 제어 모드**
- **자동 LUT 수집 모드 (AutoLUT)**
- **LUT 테이블 기반 트래킹 모드 (LUT-track)**

의 역할과 데이터 흐름, 그리고 실제 사용 방법을 정리한 문서이다.

> 기준 날짜: 2026-02-24  
> 관련 파일: `rtsp_laser_demo.cpp`, `ubuntu_tcp_server.cpp`, `pid_pwm_agent.py`, `lut_data.json`

---

## 1. 공통 구조 개요

### 1.1 프로세스 / 역할

- **Host (Ubuntu) – `rtsp_laser_demo`**
  - RTSP 카메라에서 영상 수신
  - 레이저 탐지 / 타겟 ROI 처리
  - AutoLUT 시 PWM 요청·저장 트리거
  - LUT 기반 트래킹 시 **타겟 픽셀 좌표(TU,TV)** 전송

- **중간 서버 – `ubuntu_tcp_server`**
  - `rtsp_laser_demo` 의 stdout 을 읽어 TCP로 Raspi에 전달
  - Raspi에서 오는 PWM 응답(PAN/TILT)을 FIFO로 전달 (AutoLUT용)
  - Raspi TCP 클라이언트 연결 시 FIFO `/tmp/lut_client_connected` 에 `CONNECTED` 신호 전송

- **Raspberry Pi – `pid_pwm_agent.py`**
  - TCP 클라이언트: Ubuntu 서버에 접속
  - **PID 제어 모드**: `EX,EY` 오차를 받아 PID로 PWM 제어
  - **AutoLUT 수집 모드**: PID 출력과 그리드/타겟 정보를 이용해 LUT 수집
  - **LUT-track 모드**: `lut_data.json` 을 로드하고, `TU,TV` 픽셀 좌표를 LUT로 보간해서 PWM 직접 출력
  - Kalman 필터(`Kalman2D`)로 타겟/레이저 위치 예측(필요 시)

### 1.2 텍스트 프로토콜 요약

- Host → Raspi (ubuntu_tcp_server가 변환)
  - 기본 형식:
    - `EX=...,EY=...[,TU=...,TV=...[,GR=...,GC=...]]`
  - `rtsp_laser_demo` stdout 형식 (ubuntu_tcp_server stdin):
    - `e_u e_v target_u target_v grid_r grid_c`
    - 또는 AutoLUT / LUT-track 모드에서 `e_u,e_v` 를 0으로 고정하고 `target_u,target_v,grid_r,grid_c` 만 의미 있게 사용

- Raspi → Host (AutoLUT용)
  - `REQUEST_PWM` 에 대한 응답:
    - `PAN=xxxx,TILT=yyyy`

---

## 2. PID 기반 IBVS 제어 모드

### 2.1 역할

- Host (`rtsp_laser_demo`) 에서:
  - 레이저 위치를 감지 (`VisionDetector::detectLaser`)
  - 타겟 ROI 중심과 레이저 위치 차이 `e_u, e_v` 계산
  - `IbvsController` 로 간단한 호스트 측 P 제어(디버그용)
  - Raspi 쪽에는 현재는 **주로 EX/EY, TU/TV, GR/GC만 보내는 역할**에 가깝다.

- Raspi (`pid_pwm_agent.py`) 에서:
  - 50Hz 루프에서 `ex, ey`를 읽어서 **위치형 PID** 로 PWM 계산
  - `ux_us = center_x + Kp_x * ex + Ki_x * ∫ex + Kd_x * d(ex)/dt` (y축도 동일한 구조)
  - sysfs PWM (`/sys/class/pwm/pwmchip0/pwm0`, `pwm1`) 에 duty_cycle(ns) 쓰기

### 2.2 데이터 흐름

1. `rtsp_laser_demo`:
   - 레이저 탐지 성공 시:
     - `target_u, target_v`: ROI 중심
     - `e_u = target_u - laser_x`
     - `e_v = target_v - laser_y`
   - `stdout` 에:
     - `e_u e_v target_u target_v grid_r grid_c` 출력

2. `ubuntu_tcp_server`:
   - `stdin` 한 줄을 `sscanf` 로 파싱:
     - `e_x, e_y, t_u, t_v, g_r, g_c`
   - Raspi TCP로:
     - `EX=...,EY=...,TU=...,TV=...,GR=...,GC=...` 전송

3. `pid_pwm_agent.py`:
   - `recv_loop` 에서 한 줄씩 읽고 `EXEY_RE` 정규식으로 파싱
   - 공유 상태(`SharedState`)에 `ex, ey, target_u, target_v, grid_r, grid_c` 저장
   - 50Hz 메인 루프에서:
     - `ux_us = pid_x.update(ex, dt)`
     - `uy_us = pid_y.update(ey, dt)`
     - PWM sysfs 갱신

### 2.3 Kalman 필터 적용 (개략)

- `Kalman2D`:
  - 상태: `[x, y, vx, vy]`
  - α-β 필터 형태로 **측정값과 속도를 함께 스무딩**
  - 레이저 위치나 타겟 위치를 시간상 부드럽게 예측할 때 사용 가능:
    - 예: `zx, zy` = 매 프레임 레이저 좌표
    - `update(zx, zy, dt)` → 필터링된 위치 + 다음 스텝 예측 위치 반환

---

## 3. 자동 LUT 수집 모드 (AutoLUT)

### 3.1 Host: `rtsp_laser_demo --lut-auto`

- **그리드 정의**
  - `LUT_GRID_ROWS = 11`, `LUT_GRID_COLS = 19`
  - 각 셀 중심을 하나의 LUT 포인트로 사용

- **AutoLutCalibrator**
  - 현재 셀 인덱스: `idx` → `(gr(), gc())`
  - 현재 셀 중심에 대응하는 `TargetROI` 생성:
    - `currentTarget(W, H)` 에서 `(gc+0.5) * W/cols`, `(gr+0.5) * H/rows`
  - **수렴 판정**:
    - 조건: `|e_u|,|e_v| ≤ SETTLE_THRESH_PX` (기본 6px)
    - 시간을 `settle_accum_s` 에 누적
    - `SETTLE_TIME_SEC` (예: 3초) 이상이면 “수렴”으로 간주

- **PWM 요청 및 LUT 저장 흐름**

1. Host가 AutoLUT 모드에서, 특정 셀에 대해 수렴 조건 만족:
   - `lut.updateConvergence(e_u, e_v, dt)` 가 `true` 를 반환
2. Host는 Raspi에 현재 PWM 요청:
   - `REQUEST_PWM,GR=X,GC=Y` 를 `stdout` 으로 송신
3. `ubuntu_tcp_server`:
   - `REQUEST_PWM,...` 라인은 가공 없이 TCP로 그대로 전달
4. Raspi `pid_pwm_agent.py`:
   - `recv_loop` 에서 `REQUEST_PWM_RE` 로 매치
   - `shared.last_ux_us`, `shared.last_uy_us` 를 읽어
   - `PAN=xxxx,TILT=yyyy` 형태로 TCP로 응답
5. Host:
   - `/tmp/lut_pwm_response` FIFO 에서 `PAN/TILT` 응답을 읽음
   - `lut_data.json` 에
     - `{"grid_r":r,"grid_c":c,"target_u":...,"target_v":...,"pan_us":...,"tilt_us":...}`
     - 형태로 한 포인트를 **즉시 저장 (전체 파일 리라이트)**
   - 다음 셀(`idx+1`) 로 넘어감

- **수동 레이저 위치 지정 (마우스 클릭)**
  - AutoLUT 중 레이저 탐지가 어려운 셀의 경우:
    - 마우스로 이미지 상 **레이저가 실제 있는 위치**를 클릭
    - `g_manual_laser_pending=true`, `g_manual_laser_pt` 설정
    - 다음 프레임에서:
      - `laser.found = true`, `laser.point = g_manual_laser_pt`
      - AutoLUT는 이 좌표를 기준으로 바로 PWM 요청·저장 수행
      - 이 프레임에서는 일반 EX/EY 전송을 스킵 (`skip_periodic_send=true`)

### 3.2 Raspi: `pid_pwm_agent.py --lut-mode`

- PID 제어를 사용하여 실제 레이저를 타겟 중앙에 수렴시키고,  
  각 셀마다 “좋은 상태”일 때 AutoLUT가 PWM을 가져가도록 돕는 모드.

- `LutCollector`:
  - Raspi에서 자체적으로 LUT를 수집하고 JSON으로 저장할 수 있는 구조도 있으나,
  - 최신 구조에서는 **주요 LUT 저장 책임은 Host 측(`AutoLutCalibrator`)에 있음**.
  - Raspi 쪽 LUT 수집은 필요 시 보조 용도로만 사용.

---

## 4. LUT 테이블 기반 트래킹 모드 (LUT-track)

### 4.1 Host: `rtsp_laser_demo --lut-track`

- 키 포인트:
  - **레이저 탐지 / 오차 계산을 하지 않는다.**
  - 사용자는 마우스로 타겟 박스를 그리기만 한다.
  - **박스 중심 픽셀 좌표 (TU,TV)와 대응 그리드 인덱스(GR,GC)** 만 Raspi로 보낸다.

- 동작 흐름:

1. `targetProvider` (InteractiveBoxTargetProvider):
   - 마우스로 빈 곳 드래그 → ROI 생성
   - ROI 안 드래그 → ROI 이동

2. 메인 루프에서:

   - `TargetROI targetROI = targetProvider->getTarget(frame);`
   - `if (g_lut_track)` 분기:

     ```cpp
     if (g_lut_track)
     {
         if (targetROI.valid)
         {
             // 1) 박스와 LUT 격자 그리기
             // 2) 박스 중심 (target_u, target_v) 계산
             // 3) GR, GC 계산
             // 4) client_connected && frame_id % SEND_EVERY_N == 0 인 프레임마다
             //    "0 0 target_u target_v GR GC" 를 stdout으로 전송
         }

         cv::imshow(...);
         waitKey(1);
         continue; // 이 모드는 레이저 탐지/AutoLUT 로직 완전히 스킵
     }
     ```

   - Host → ubuntu_tcp_server:
     - 예: `0 0 960 540 5 9`
       - `EX=0,EY=0,TU=960,TV=540,GR=5,GC=9` 로 변환되어 Raspi로 전송됨.

### 4.2 Raspi: `pid_pwm_agent.py --lut-track`

- 실행 예:

```bash
python3 pid_pwm_agent.py \
  --host 192.168.x.x --port 5555 \
  --lut-track \
  --lut-out lut_data.json \
  --frame-w 1920 --frame-h 1080
```

- 시작 시:
  - `lut_table = LutTable.from_json(args.lut_out)`
    - `rows, cols, pan[r][c], tilt[r][c], valid[r][c]` 생성
    - Host에서 AutoLUT로 만든 `lut_data.json` 사용

- 메인 루프:

1. `recv_loop` 가 Host에서 오는 `EX/EY/TU/TV/GR/GC` 를 수신
   - `EXEY_RE` 로 파싱
   - `SharedState` 에 `target_u`, `target_v` 업데이트

2. 50Hz 제어 루프에서:

   - `with shared.lock:` 에서 `tu, tv` 읽기

   - `if args.lut_track and lut_table is not None:` 분기:

     ```python
     if args.lut_track and lut_table is not None:
         if had_update:
             pan_us, tilt_us, ok = lut_table.interpolate(
                 tu, tv, args.frame_w, args.frame_h
             )
             if ok:
                 ux_us = float(pan_us)
                 uy_us = float(tilt_us)
                 write_pwm_duty(...)
                 shared.last_ux_us = ux_us
                 shared.last_uy_us = uy_us
     ```

   - 이 모드에서는:
     - `EX/EY` 는 **무시** (0으로 보내도 상관 없음)
     - PID, LutCollector, auto-tune, PIDLOG 는 비활성 또는 영향 거의 없음
     - **LUT + 2D 보간** 만으로 PWM을 직접 만든다.

- 보간 방식 (`LutTable.interpolate`) 요약:

  - 최신 버전은 **cubic B-spline 4x4 보간**을 사용:
    - 픽셀 (TU,TV) → 연속 그리드 좌표 (cf,rf)
    - 그 주변 4x4 컨트롤 포인트에서
      - B-spline basis `b0..b3` 로 가중치 계산
      - `pan`, `tilt` 에 대해 2D 합산
    - 4x4 영역 안에 `valid=False` 인 셀이 하나라도 있으면 `ok=False` 로 실패 처리

---

## 5. AutoLUT 중 수동 클릭 저장 플로우 정리

### 5.1 문제 상황

- 어떤 셀에서는 레이저가 약하거나, 배경/반사 때문에 자동 탐지가 실패할 수 있음.
- 이때 사용자가 해당 셀을 그냥 건너뛰면 LUT에 구멍이 생김.

### 5.2 해결: 수동 클릭 기반 PWM 저장

1. AutoLUT 모드에서 레이저가 안 잡힐 때:
   - 사용자가 영상 창에서 **레이저가 보이는 픽셀을 직접 클릭**

2. `rtsp_laser_demo.cpp`:
   - 마우스 콜백에서:
     - `g_manual_laser_pending = true`
     - `g_manual_laser_pt = clicked_point`

3. 다음 프레임:
   - AutoLUT 로직에서:

   ```cpp
   DetectionResult laser = detector.detectLaser(frame);
   bool used_manual_laser = false;
   if (g_auto_lut && g_manual_laser_pending)
   {
       laser.found = true;
       laser.point = g_manual_laser_pt;
       used_manual_laser = true;
   }

   if (g_auto_lut && client_connected && targetROI.valid && laser.found)
   {
       if (used_manual_laser)
       {
           lut_just_advanced = lut.requestPwmAndSave(target_u, target_v, std::cout);
           g_manual_laser_pending = false;
           skip_periodic_send = true; // 이 프레임은 일반 EX/EY 전송 스킵
       }
       else if (lut.updateConvergence(...))
       {
           ...
       }
   }
   ```

4. `requestPwmAndSave`:
   - 평소와 동일하게 `REQUEST_PWM` → Raspi 응답 → `lut_data.json` 저장
   - 이 셀은 “수동 보정된 레이저 위치” 기반으로 LUT 포인트가 기록됨.

---

## 6. 요약 및 사용 팁

- **PID 모드**:
  - 빠른 개발/튜닝용, AutoLUT 수집 시 레이저를 타겟에 최대한 맞추는 역할.
  - `--kp-x`, `--ki-x`, `--kp-y`, `--ki-y` 등을 조정해 따라오는 느낌 튜닝.
  - Kalman 필터(`Kalman2D`)로 레이저/타겟 위치 스무딩 가능.

- **AutoLUT 모드 (`rtsp_laser_demo --lut-auto`)**:
  - 화면 전체를 그리드로 순회하며 LUT를 자동으로 수집.
  - 수렴 조건: `|e|<=SETTLE_THRESH_PX` 를 `SETTLE_TIME_SEC` 동안 유지.
  - 수동 클릭 기능으로 어려운 셀도 커버 가능.

- **LUT-track 모드**:
  - Host:
    - `--lut-track` 로 실행
    - 마우스로 타겟 박스만 지정하면, 박스 중심 픽셀이 자동으로 Raspi로 전송.
  - Raspi:
    - `--lut-track --lut-out lut_data.json --frame-w 1920 --frame-h 1080`
    - AutoLUT로 만들어진 `lut_data.json` 기반으로 2D 보간(PWM) 수행.
  - PID 없이도 **“한 번 캘리브레이션 해 놓은 LUT”만으로 빠르게 타겟으로 이동**하는 모드.

이 문서는 `pid_pwm_agent.py`, `rtsp_laser_demo.cpp`, `ubuntu_tcp_server.cpp`, `lut_data.json` 의 현재 구현을 기준으로 작성되었다.  
LUT가 화면 전체(특히 상·하단)까지 충분히 채워질수록 LUT-track 모드에서의 위치 오차가 줄어든다. 추후 필요하다면 LUT의 경계부에서 1D 보간 또는 extrapolation 전략을 추가해도 된다.

---

## 7. `--nolut` 모드 (레이저 탐지 스트림/지연 측정용)

### 7.1 목적

- **그리드/LUT 기능을 끈 상태**로 레이저 탐지 결과를 “가볍게” 흘려보내는 모드.
- 특히 `Laser_Detection_Delay.py`처럼 “레이저 ON → 호스트가 레이저를 감지해 패킷을 보내기까지”의 지연을 측정할 때 사용.

### 7.2 동작 요약

- Host: `rtsp_laser_demo --nolut --gst`
  - `g_lut_mode = false`
  - 레이저가 잡히면 **박스(ROI)가 없어도** 주기적으로 한 줄을 `stdout`으로 보냄
    - (지연 측정용이라 `EX/EY`는 0으로 보내도 충분)
    - `TU/TV`에는 레이저 픽셀 좌표를 넣음
- Server: `ubuntu_tcp_server`
  - Host stdout(공백 구분 6개 숫자)을 받아 Raspi로 `EX=...,EY=...,TU=...,TV=...,GR=...,GC=...` 형태로 전송
- Raspi: `Laser_Detection_Delay.py`
  - TCP로 들어오는 라인에서 `EX=...,EY=...`가 수신되는 시각을 잡아 지연(ms)으로 계산

### 7.3 실행 예

Host (Ubuntu):

```bash
cd hardware/stm32-laser/host_cpp/build
./rtsp_laser_demo --nolut --gst | ../../tmp_server/ubuntu_server/ubuntu_tcp_server
```

Raspi:

```bash
sudo python3 hardware/Raspi-laser/Laser_Detection_Delay.py --host <UBUNTU_IP> --port 5555
```

> 참고: `--nolut` 모드에서는 “박스 생성”을 하지 않아도 레이저만 잡히면 전송되므로, 지연 측정이 편하다.

---

## 8. `--lut-check` 모드 (LUT PWM 검증/그리드 셀 일치 확인)

### 8.1 목적

- Host에 저장된 `lut_data.json`의 각 그리드 포인트(`pan_us`, `tilt_us`)가 실제로
  - 라즈베리 파이/서보에 적용됐을 때
  - 레이저가 “해당 그리드 셀”로 들어오는지
  를 빠르게 확인하는 **검증 모드**.

### 8.2 핵심 아이디어

- AutoLUT/LUT-track은 “오차 기반” 또는 “픽셀 기반 보간”이라 디버깅이 복잡할 수 있음.
- LUT-check는 아예 **LUT에 저장된 PWM을 그대로 강제 적용(SET_PWM)**하고,
  레이저 탐지 결과가 목표 셀에 들어오는지 화면에서 체크한다.

### 8.3 프로토콜(추가)

- Host → Raspi:
  - `SET_PWM,PAN=####,TILT=####[,GR=..,GC=..]`
- Raspi(`pid_pwm_agent.py`):
  - `SET_PWM`을 받으면 해당 PWM을 **즉시 적용**(PID/LUT-track보다 우선)

### 8.4 실행 예

Host (Ubuntu):

```bash
cd hardware/stm32-laser/host_cpp/build
./rtsp_laser_demo --gst --lut-check ./lut_data.json | ../../tmp_server/ubuntu_server/ubuntu_tcp_server
```

Raspi:

```bash
sudo python3 hardware/Raspi-laser/pid_pwm_agent.py --host <UBUNTU_IP> --port 5555
```

### 8.5 화면/키 조작

- Host 창에서:
  - `n`: 다음 LUT 포인트(다음 그리드 셀)로 이동 + 해당 PWM을 Raspi로 전송
  - `p`: 이전 LUT 포인트
  - `d`: 빨간색 마스크 디버그(`red_mask`) 토글
  - `q` 또는 `ESC`: 종료

### 8.6 판정(표시)

- 목표 셀: **노란 박스**
- 레이저가 검출되면 레이저 좌표로 현재 셀을 계산해서
  - 목표 셀과 일치하면 목표 셀 박스를 **초록색**으로 한 번 더 표시
  - 레이저 점은 **빨간 원**으로 표시


