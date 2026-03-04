# STM32 IBVS PID 제어 구현 정리 (2026-03-04 기준)

## 1. 전체 아키텍처

- **호스트 (PC, `rtsp_laser_demo`)**
  - 카메라 영상에서:
    - 바운딩 박스(타겟) 중심 좌표
    - 레이저 포인트 좌표
  - 를 계산하고 두 점의 차이:
    - \( e_u = x_\text{target} - x_\text{laser} \)
    - \( e_v = y_\text{target} - y_\text{laser} \)
  - 를 **픽셀 오차**로 만든다.
  - 일정 프레임마다(예: 5프레임에 한 번) 표준 출력으로:
    - `"e_u e_v\n"` 형식으로 내보냄.

- **Ubuntu TCP 서버 (`ubuntu_tcp_server`)**
  - 파이프 입력 `"e_u e_v"` 를 읽어서
  - ESP/STM32 쪽으로는:
    - `EX=...,EY=...\n`
  - 형식으로 전송.

- **STM32 (`Core/Src/main.c`)**
  - ESP8266 으로부터 들어오는 `+IPD,...:EX=...,EY=...` 문자열을 파싱해서
    - `ibvs_err_x`, `ibvs_err_y` 에 픽셀 오차를 저장.
  - 내부 타이머 기준으로 약 **50Hz 주기**로 `IbvsPid_Update()` 를 호출해서
    - 서보 PWM(us) 를 갱신한다.

요약하면, **센서/비전(픽셀 오차)은 PC**,  
**제어기(PID)와 액추에이터(PWM)는 STM32** 에서 담당한다.

---

## 2. STM32 쪽 전역 상태 구조

### 2.1 픽셀 오차 입력

- `main.c` 전역:
  - `ibvs_err_x`, `ibvs_err_y`:
    - 호스트에서 들어온 **X/Y 픽셀 오차** (EX, EY).
  - `ibvs_err_valid`:
    - 최신 오차가 유효한지 여부(0/1).
  - `ibvs_last_err_tick`:
    - 마지막으로 EX/EY 를 수신한 시각(ms, `HAL_GetTick()` 기준).

- EX/EY 파싱 (WiFi +IPD 처리 블록 내부, 한 줄 단위 파서에서):
  ```c
  float ex_f = 0.0f, ey_f = 0.0f;
  ...
  if (sscanf(cursor, "EX=%f,EY=%f", &ex_f, &ey_f) == 2)
  {
    ibvs_err_x         = ex_f;
    ibvs_err_y         = ey_f;
    ibvs_err_valid     = 1;
    ibvs_last_err_tick = HAL_GetTick();
  }
  ```

### 2.2 PID 축 상태 구조체

- 전역 구조체:
  ```c
  typedef struct
  {
    float kp;
    float ki;
    float kd;
    float integ;
    float prev_err;
    float out_us;
  } IbvsPidAxis;

  static IbvsPidAxis pid_x; /* PAN  (PA0 / TIM2_CH1) */
  static IbvsPidAxis pid_y; /* TILT (PA8 / TIM1_CH1) */
  ```

- 공통 초기화 함수:
  ```c
  static void IbvsPid_AxisInit(IbvsPidAxis *a, float kp, float ki, float kd, float initial_us)
  {
    a->kp      = kp;
    a->ki      = ki;
    a->kd      = kd;
    a->integ   = 0.0f;
    a->prev_err= 0.0f;
    a->out_us  = initial_us;
  }
  ```

### 2.3 PID 주기 및 타임아웃

- 전역 변수:
  ```c
  static uint32_t ibvs_last_update_tick = 0;
  #define IBVS_UPDATE_PERIOD_MS  20u   /* IBVS PID 업데이트 주기 ≈ 50Hz */
  #define IBVS_ERR_TIMEOUT_MS   500u   /* 이 시간 동안 새 오차가 없으면 PID 정지 */
  ```

- 메인 루프 안에서:
  - **오차 타임아웃**:
    - `ibvs_err_valid == 1` 인 상태에서
    - `now - ibvs_last_err_tick > IBVS_ERR_TIMEOUT_MS` 이면
      - `ibvs_err_valid = 0;` 로 바꾸어 **더 이상 PID를 돌리지 않음**.
  - **주기적 업데이트**:
    - `dt_ms = now - ibvs_last_update_tick` 가 `IBVS_UPDATE_PERIOD_MS` 이상이면:
      - `dt_sec = dt_ms / 1000.0f` 로 환산 후 `IbvsPid_Update(dt_sec)` 호출.

---

## 3. IBVS PID 초기화 로직

### 3.1 현재 PWM에서 초기값 읽기

- `IbvsPid_Init()` 에서, 이미 `Servo_Init()` 이 끝난 상태라고 가정하고:
  - 실제 타이머 비교 레지스터(CCR) 값에서 초기 PWM(us) 를 읽어온다.
  ```c
  uint32_t ux_init = __HAL_TIM_GET_COMPARE(&htim2, TIM_CHANNEL_1); /* PA0 (PAN, X) */
  uint32_t uy_init = __HAL_TIM_GET_COMPARE(&htim1, TIM_CHANNEL_1); /* PA8 (TILT, Y) */
  if (ux_init < PWM_US_MIN || ux_init > PWM_US_MAX) ux_init = 1430u;
  if (uy_init < PWM_US_MIN || uy_init > PWM_US_MAX) uy_init = 1250u;
  ```

### 3.2 축별 게인 및 부호 설정

- 그 다음, 축별 PID를 초기화:
  ```c
  // X축(PAN, PA0): 오른쪽으로 갈수록 PWM 증가 → Kp_x > 0
  // Y축(TILT, PA8): 아래로 갈수록 PWM 감소 → Kp_y < 0
  IbvsPid_AxisInit(&pid_x,  0.02f, 0.0f, 0.0f, (float)ux_init);
  IbvsPid_AxisInit(&pid_y, -0.02f, 0.0f, 0.0f, (float)uy_init);

  ibvs_err_x         = 0.0f;
  ibvs_err_y         = 0.0f;
  ibvs_err_valid     = 0;
  ibvs_last_err_tick = 0;
  ibvs_last_update_tick = HAL_GetTick();
  ```

- 현재는 `Ki = Kd = 0` 으로 설정되어 있어서, **실질적으로는 P 제어만 활성화된 상태**이다.

---

## 4. PID 업데이트 식 (현재 구현)

### 4.1 업데이트 호출 조건

- 메인 루프에서:
  ```c
  if (ibvs_err_valid && (now - ibvs_last_err_tick) > IBVS_ERR_TIMEOUT_MS)
    ibvs_err_valid = 0;  // 오차 오래 안 오면 PID 정지

  uint32_t dt_ms = now - ibvs_last_update_tick;
  if (dt_ms >= IBVS_UPDATE_PERIOD_MS)
  {
    float dt_sec = (float)dt_ms / 1000.0f;
    ibvs_last_update_tick = now;
    IbvsPid_Update(dt_sec);
  }
  ```

- 즉, **유효한 오차가 있고**, **20ms 정도 시간이 지났을 때마다** `IbvsPid_Update()` 가 호출된다.

### 4.2 축별 PID 계산 (P-only 상태)

- `IbvsPid_Update(dt_sec)` 내부:
  ```c
  if (!ibvs_err_valid)
    return;
  if (dt_sec <= 0.0001f)
    return;

  float ex = ibvs_err_x;  // X축 픽셀 오차
  float ey = ibvs_err_y;  // Y축 픽셀 오차

  // X축 (PAN, PA0)
  pid_x.integ   += ex * dt_sec;
  float derr_x   = (ex - pid_x.prev_err) / dt_sec;
  float du_x     = pid_x.kp * ex + pid_x.ki * pid_x.integ + pid_x.kd * derr_x;
  pid_x.out_us  += du_x;
  pid_x.out_us   = clampf(pid_x.out_us, (float)PWM_US_MIN, (float)PWM_US_MAX);
  pid_x.prev_err = ex;

  // Y축 (TILT, PA8)
  pid_y.integ   += ey * dt_sec;
  float derr_y   = (ey - pid_y.prev_err) / dt_sec;
  float du_y     = pid_y.kp * ey + pid_y.ki * pid_y.integ + pid_y.kd * derr_y;
  pid_y.out_us  += du_y;
  pid_y.out_us   = clampf(pid_y.out_us, (float)PWM_US_MIN, (float)PWM_US_MAX);
  pid_y.prev_err = ey;

  // 실제 PWM 적용: PA8=Y, PA0=X
  uint32_t uy = (uint32_t)pid_y.out_us;
  uint32_t ux = (uint32_t)pid_x.out_us;
  Servo_SetAllUs(uy, ux);
  ```

- 현재 `Ki = Kd = 0` 이므로 실제로는:
  - `pid_x.out_us += Kp_x * ex;`
  - `pid_y.out_us += Kp_y * ey;`
  - 와 같은 **증분형 P 제어**가 20ms 주기마다 반복되는 형태이다.

> 참고: 호스트에서 EX/EY를 5프레임마다 한 번씩 보내고,  
> STM32 PID는 20ms마다 계속 도는 구조이기 때문에,  
> 같은 EX/EY가 여러 번 적용되어 **체감상 더 공격적인 P 제어**처럼 보일 수 있다.

---

## 5. 요약 및 향후 조정 포인트

- **요약**
  - PC: 바운딩 박스 중심 vs 레이저 위치 → 픽셀 오차 EX/EY 계산.
  - STM32:
    - +IPD 라인에서 EX/EY 파싱 → `ibvs_err_x/ibvs_err_y` 업데이트.
    - 약 50Hz 주기로 `IbvsPid_Update()` 호출.
    - 현재는 **P-only(증분형)** 제어이며,
      - X축: Kp_x = +0.02, PAN(오른쪽으로 갈수록 PWM 증가).
      - Y축: Kp_y = -0.02, TILT(아래로 갈수록 PWM 감소).
    - 유효한 오차가 500ms 이상 들어오지 않으면 PID 정지.

- **향후 개선 아이디어**
  - **직접형 P 제어**( `out_us = neutral + Kp * e` ) 로 바꿔서 오버슈트를 줄이거나,
  - `IBVS_UPDATE_PERIOD_MS` 를 조정해서 내부 PID 업데이트 주파수를 낮추는 방식으로
    **“너무 공격적인” 동작을 완화**할 수 있다.
  - 이후 단계에서 `Ki`, `Kd` 를 조금씩 키워가며 steady-state 오차/오버슈트/진동 특성을 튜닝할 수 있다.

