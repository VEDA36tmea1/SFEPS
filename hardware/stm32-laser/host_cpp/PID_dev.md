# IBVS PID 제어 개발 노트

## 1. 현재 상태 – P 제어만 사용

- 현재 `IbvsController` 는 **순수 P 제어**만 구현되어 있다.
  - 입력: 픽셀 좌표 기준 오차 \((e_u, e_v)\)
  - 출력: 서보 PWM 마이크로초 값 \((u_\text{pan}, u_\text{tilt})\)
  - 업데이트 식(개념):
    \[
    u_{k+1} = u_k + K_p \cdot e_k
    \]
- 장점:
  - 구현이 단순하고 직관적이다.
  - 초기 bring-up(방향 확인, 배선/매핑 검증) 단계에서는 충분하다
  .

하지만 **실제 객체(레이저 점)를 바운딩 박스 중심으로 맞추는 “트래킹”** 관점에서 보면,
P 제어만으로는 다음과 같은 한계가 드러난다.

## 2. P 제어의 한계 – 왔다 갔다 / 느린 추종 / 정확도 문제

### 2.1 왔다 갔다(진동, overshoot)

- **오차가 크면** → \(K_p \cdot e\) 항이 커져서 **PWM 변화량이 컸다가**,  
  목표 근처에 오면 **오차 방향이 뒤집히면서 다시 반대쪽으로 움직이는** 패턴이 반복된다.
- 카메라 프레임 지연, 서보의 관성, 링크 기구의 유격(백래시)까지 합쳐지면,
  “타겟 근처에서 좌우로 왔다 갔다” 하는 진동이 발생하기 쉽다.

### 2.2 느린 추종 / 정특성 오차(steady-state error)

- **gain \(K_p\) 를 너무 키우면**:
  - 빠르게 목표에 도달하지만 진동/overshoot 이 심해진다.
- **gain \(K_p\) 를 너무 줄이면**:
  - 진동은 줄어드나, 서보 분해능/마찰 때문에  
    작은 오차에 대해서는 실제 PWM 변화가 거의 0이 되어
    **항상 일정 픽셀 오차를 남기고 멈추는 정특성 오차**가 커진다.
- 결과적으로:
  - “타겟을 찾긴 하는데 속도가 느리다”
  - “중앙까지 정확히 오지 않고 항상 약간 왼쪽/위에서 멈춘다”
  같은 느낌이 생긴다.

### 2.3 노이즈/측정 흔들림

- 카메라 기반 레이저 검출은 프레임마다 **몇 픽셀 정도의 잡음**이 섞일 수 있다.
- P 제어만 있을 때, 이 잡음이 그대로 PWM 명령에 반영되면
  - 레이저 점이 타겟 근처에서도 미세하게 “떨리는” 모션이 나온다.

## 3. 보완 방향 – PID (P + I + D) 제어 도입 아이디어

목표: **추종 속도는 유지하면서, steady-state 오차를 줄이고, 진동/노이즈를 억제**하는 것.

### 3.1 I(적분) 항 도입 – steady-state 오차 보정

- 개념:
  \[
  I_k = I_{k-1} + e_k \cdot \Delta t
  \]
  \[
  u_{k+1} = u_k + K_p e_k + K_i I_k
  \]
- 역할:
  - P 제어만으로는 남게 되는 **지속적인 작은 오차(항상 약간 왼쪽/위)** 를  
    적분 항이 조금씩 누적해서 보정해 준다.
  - 충분한 시간이 지나면 오차 평균이 0에 가깝게 수렴하도록 만든다.
- 구현 시 주의점:
  - **적분 항 클램프(anti-windup)** 필요:
    - \(I_k\) 자체를 \([\text{I\_MIN}, \text{I\_MAX}]\) 로 제한
    - 또는 \(K_i I_k\) 가 만들 수 있는 PWM 보정량을 제한
  - 카메라 끊김/타겟 상실 등에서 오랫동안 큰 오차가 누적되지 않도록,
    - 타겟이 유효하지 않을 때는 적분을 멈추거나,
    - 오차가 일정 범위 밖이면 적분 리셋/서서히 감소시키는 로직 필요.

### 3.2 D(미분) 항 도입 – overshoot/진동 감쇠

- 개념:
  \[
  D_k = \frac{e_k - e_{k-1}}{\Delta t}
  \]
  \[
  u_{k+1} = u_k + K_p e_k + K_i I_k + K_d D_k
  \]
- 역할:
  - **오차 변화율**(error slope)을 보고, 빠르게 접근할수록 브레이크를 건다.
  - 타겟에 도달하기 직전에 제동이 걸려서, overshoot 와 진동이 줄어든다.
- 구현 시 주의점:
  - 픽셀 노이즈에 매우 민감하므로, 미분 전에 간단한 **저역통과 필터**를 쓰거나,
  - \(D_k\) 에 대한 별도 low-pass 필터(예: 1차 IIR)를 넣는 편이 좋다.

### 3.3 실질적인 튜닝 순서 제안

1. **P부터 안정적으로**
   - 현재처럼 P-only 로 놓고, 진동 없이 따라올 수 있는 가장 큰 \(K_p\) 를 찾는다.
   - 이때 마찰/해상도 때문에 남는 정특성 오차(픽셀)를 기록한다.
2. **I 추가 (작게 시작)**
   - 작은 \(K_i\) 로 시작해서, steady-state 오차가 서서히 0 에 가까워지는지 확인.
   - overshoot/진동이 커지면:
     - \(K_i\) 를 줄이거나,
     - 적분 클램프를 더 타이트하게 조정.
3. **D 추가 (필요할 때)**
   - 오버슈트/진동이 여전히 크다면 \(K_d\) 를 조금씩 올려서 감쇠 효과를 본다.
   - 레이저 위치 노이즈 때문에 움직임이 들쑥날쑥해지면,
     - \(K_d\) 를 줄이거나,
     - 오차/레이저 좌표에 저역통과 필터를 추가.

## 4. 향후 구현 계획(요약)

- `IbvsController` 에 다음 상태를 추가:
  - 축별 적분 상태 \((I_u, I_v)\)
  - 이전 오차 \((e_u^\text{prev}, e_v^\text{prev})\)
- `update(e_u, e_v, dt)` 내부에서:
  - P, I, D 항을 각각 계산하고 합산
  - 축별로 **PWM 변화량 clamp + 전체 PWM 범위 clamp** 적용
- `rtsp_laser_demo` 에서:
  - 프레임당 `dt` 를 이미 가지고 있으므로, 이를 PID 에 그대로 전달
  - PID 튜닝을 쉽게 하기 위해 dev 노트에 \(K_p, K_i, K_d\) 실험 로그를 계속 기록

현재는 **P 제어만으로 기본 추종이 되는 상태**까지 확인한 단계이고,  

---

## 2026-03-04 – PID 제어를 STM32로 옮기고, PC에서는 오차만 보내도록 구조 변경

### 1. 아키텍처 변경 개요

- **이전 구조**
  - PC(`rtsp_laser_demo`)에서:
    - 바운딩 박스 중심과 레이저 포인트 차이 \((e_u, e_v)\)를 계산.
    - `IbvsController` 로 P 제어 수행 후, **서보 PWM(us)을 직접 계산**.
    - stdout 으로 `"PAN_US TILT_US\n"` (예: `1500 1400`) 을 매 프레임(또는 N프레임마다) 출력.
  - Ubuntu TCP 서버(`ubuntu_tcp_server`)에서:
    - `"1500 1400"` 을 읽어 `CX=...,CY=...` 포맷으로 ESP/STM32에 전달.
  - STM32(`main.c`)에서:
    - `+IPD, ... CX=...,CY=...` 를 파싱해 **PWM을 그대로 적용**하는 구조 (PC가 “외부 컨트롤러” 역할).

- **현재 구조 (2026-03-04 기준)**
  - PC(호스트)는 **픽셀 오차 \((e_u, e_v)\)만 계산해서 전송**하고,
  - **PID 제어(및 최종 PWM 계산)는 STM32 내부에서 수행**한다.
  - 즉, 역할을 명확히 나눔:
    - **PC**: Vision + 에러 계산(센서/관측 레벨)
    - **STM32**: PID + 서보 구동(제어기/액추에이터 레벨)

### 2. 호스트 코드 변경 내용

#### 2.1 `rtsp_laser_demo.cpp` – PWM 대신 픽셀 오차만 전송

- 기존:
  - `IbvsController` 로 P 제어 후:
    ```cpp
    IbvsOutput out = controller.update(e_u, e_v, dt_sec);
    std::cout << out.pan_us << " " << out.tilt_us << std::endl; // "1500 1400"
    ```
- 변경:
  - P 제어(PWM 계산)는 **디버그용**으로만 남기고,
  - 파이프라인(stdout)으로는 **픽셀 오차만** 전송:
    ```cpp
    // 에러: 타겟 - 레이저 (픽셀)
    double e_u = static_cast<double>(targetROI.center().x - laser.point.x);
    double e_v = static_cast<double>(targetROI.center().y - laser.point.y);

    IbvsOutput out = controller.update(e_u, e_v, dt_sec); // 디버그용

    // N 프레임마다 픽셀 오차만 출력 (예: "15.0 -10.0")
    constexpr int SEND_EVERY_N_FRAMES = 5;
    if (frame_id % SEND_EVERY_N_FRAMES == 0)
    {
        std::cout << e_u << " " << e_v << std::endl;
    }
    ```
- 요약:
  - **표준 출력 → (e_u, e_v)** 로 변경.
  - stderr 로는 여전히 `target/laser/e_u/e_v` 와 호스트측 P제어 결과(PWM)를 남겨,  
    튜닝/디버깅에 활용 가능하도록 유지.

#### 2.2 `ubuntu_tcp_server.cpp` – CX/CY → EX/EY 포맷으로 변경

- 기존:
  - `"x y"` 형식을 받으면 `CX=...,CY=...` 형태로 ESP/STM32 로 전송:
    ```cpp
    if (std::sscanf(trimmed.c_str(), "%f %f", &x, &y) == 2) {
        int len = std::snprintf(send_buf, sizeof(send_buf),
                                "CX=%.6f,CY=%.6f\n", x, y);
        ...
    }
    ```
- 변경:
  - `"x y"` 를 **픽셀 오차(또는 일반적인 실수 두 개)** 라고 해석하고,
  - `EX=...,EY=...` 포맷으로 전송:
    ```cpp
    if (std::sscanf(trimmed.c_str(), "%f %f", &x, &y) == 2) {
        int len = std::snprintf(send_buf, sizeof(send_buf),
                                "EX=%.6f,EY=%.6f\n", x, y);
        ...
    }
    ```
- 요약:
  - `"e_u e_v"` → `"EX=...,EY=..."` → ESP/STM32 로 전달.
  - 기존의 `CX/CY`·정수 `"1500 1400"` 전송 경로는 **수동 디버그용**으로 남겨둘 수 있음.

### 3. STM32 `main.c` – PID 제어기 및 EX/EY 파서 추가

#### 3.1 전역 상태 및 PID 축 구조체

- 전역 변수:
  ```c
  static float     ibvs_err_x      = 0.0f;
  static float     ibvs_err_y      = 0.0f;
  static uint8_t   ibvs_err_valid  = 0;
  static uint32_t  ibvs_last_err_tick = 0;  /* 마지막으로 오차를 받은 시각(ms) */

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

  static uint32_t    ibvs_last_update_tick = 0;
  #define IBVS_UPDATE_PERIOD_MS  20u   /* IBVS PID 업데이트 주기 ≈ 50Hz */
  #define IBVS_ERR_TIMEOUT_MS  500u    /* 이 시간 동안 새 오차가 없으면 PID 정지 */
  ```

- 축별 PID 초기화 함수:
  ```c
  static void IbvsPid_AxisInit(IbvsPidAxis *a, float kp, float ki, float kd, float initial_us)
  {
    if (!a) return;
    a->kp      = kp;
    a->ki      = ki;
    a->kd      = kd;
    a->integ   = 0.0f;
    a->prev_err= 0.0f;
    a->out_us  = initial_us;
  }
  ```

- 전체 IBVS PID 초기화:
  ```c
  static void IbvsPid_Init(void)
  {
    // STM32 Servo_Init 과 동일한 초기 PWM:
    // PAN(X) = PA0/TIM2_CH1 ≈ 1430us, TILT(Y) = PA8/TIM1_CH1 ≈ 1250us
    // X: 오른쪽으로 갈수록 PWM 증가 → Kp_x > 0
    // Y: 아래로 갈수록 PWM 감소   → Kp_y < 0
    IbvsPid_AxisInit(&pid_x, 0.02f, 0.0f, 0.0f, 1430.0f);
    IbvsPid_AxisInit(&pid_y, -0.02f, 0.0f, 0.0f, 1250.0f);

    ibvs_err_x = 0.0f;
    ibvs_err_y = 0.0f;
    ibvs_err_valid = 0;
    ibvs_last_err_tick = 0;
    ibvs_last_update_tick = HAL_GetTick();
  }
  ```

- `main()` 초기화 시:
  ```c
  Servo_Init();
  ...
  Led_Init();
  Led_SetAutoMode(control_mode == MODE_AUTO);
  IbvsPid_Init();  // IBVS PID 초기화
  ```

#### 3.2 +IPD 파서 – EX/EY → 오차 입력, CX/CY·정수는 기존처럼 PWM 직접 제어

- 기존 `+IPD` 처리 블록에서, 줄 단위 파싱 루프 안에 **우선순위**를 추가:
  ```c
  /* 형식 0: EX=...,EY=... (픽셀 오차) */
  float ex_f = 0.0f, ey_f = 0.0f;
  float cx_f = 0.0f, cy_f = 0.0f;
  if (sscanf(cursor, "EX=%f,EY=%f", &ex_f, &ey_f) == 2)
  {
    ibvs_err_x         = ex_f;
    ibvs_err_y         = ey_f;
    ibvs_err_valid     = 1;
    ibvs_last_err_tick = HAL_GetTick();
  }
  else if (sscanf(cursor, "CX=%f,CY=%f", &cx_f, &cy_f) == 2)
  {
    // 형식 1: CX/CY → 기존 PWM 직접 명령
    ...
    Servo_SetAllUs(u2, u1); // PA8=CY, PA0=CX
  }
  else
  {
    // 형식 2: "1500 1400" 또는 "1500"
    ...
    Servo_SetAllUs((uint32_t)uy, (uint32_t)ux);
  }
  ```

- 요약:
  - **EX/EY** 가 오면 → `ibvs_err_x / ibvs_err_y` 에 저장 → PID 입력으로 사용.
  - 없으면(또는 수동 디버깅 모드) 여전히 `CX/CY` 나 `"1500 1400"` 으로 **직접 PWM 제어 가능**.

#### 3.3 메인 루프에서 주기적인 PID 업데이트

- `while(1)` 루프 안에 IBVS PID 섹션 추가:
  ```c
  {
    uint32_t now = HAL_GetTick();

    // 일정 시간 동안 새 오차가 없으면 PID 정지 (failsafe)
    if (ibvs_err_valid && (now - ibvs_last_err_tick) > IBVS_ERR_TIMEOUT_MS)
    {
      ibvs_err_valid = 0;
    }

    // 주기적으로 PID 업데이트 (예: 50Hz)
    uint32_t dt_ms = now - ibvs_last_update_tick;
    if (dt_ms >= IBVS_UPDATE_PERIOD_MS)
    {
      float dt_sec = (float)dt_ms / 1000.0f;
      ibvs_last_update_tick = now;
      IbvsPid_Update(dt_sec);
    }
  }
  ```

- `IbvsPid_Update()` 에서는:
  - X/Y 오차에 대해 각각:
    - 적분/미분 항 갱신 (현재는 Ki, Kd=0 → 사실상 P제어와 동일)
    - `pid_x.out_us`, `pid_y.out_us` 업데이트 후 `PWM_US_MIN/MAX` 범위로 클램프
  - `Servo_SetAllUs(uy, ux);` 호출로 실제 PA8/PA0 PWM 갱신.

### 4. 현재 상태 및 다음 단계

- **현재**
  - Vision(오차 계산)은 PC(호스트),  
    PID(제어)는 STM32 내부로 역할이 분리된 상태.
  - STM32 PID는 아직 **P만 활성화**(Ki=Kd=0) 이며,
    기존 호스트 P 제어에서 쓰던 \(K_p\) 값을 그대로 가져와 사용 중.
  - 외부 루프(비전) 주기는 카메라 FPS/`SEND_EVERY_N_FRAMES` 에 따라 결정되고,  
    내부 PID 루프는 약 50Hz (`IBVS_UPDATE_PERIOD_MS=20ms`) 로 동작.

- **다음 단계**
  - `pid_x/ pid_y` 의 `ki`, `kd` 를 하나씩 키워가며:
    - steady-state 오차 감소(I 항)
    - overshoot/진동 감쇠(D 항)
    를 실험/튜닝.
  - 튜닝 결과(좋았던 \(K_p, K_i, K_d\) 조합과 로그)를 이 문서에 순차적으로 추가 기록할 예정.

