# stm32-laser 개발/문제 해결 노트

## ⚠️서보 시그널 회로 – 어떻게 하면 되나?

3.3V MCU로 5V 서보 시그널을 만들 때, **추천하는 회로**는 아래 둘 중 하나다.

---

### 방법 1: NPN 반전 회로 (권장)

3.3V로 스위칭이 확실하고, 서보 신호선을 **HIGH/LOW** 둘 다 잘 만든다.

| 부품 | 연결 |
|------|------|
| **NPN** (2N2222, 2N3904, S8050 등) | Collector → 서보 Signal 선, Emitter → **GND** |
| **Base** | 1kΩ~4.7kΩ → PA0 (또는 PA8) |
| **서보 Signal ↔ +5V** | 10kΩ 풀업 (R3) |
| **공통** | 서보 GND, Nucleo GND, 5V 어댑터 GND 한 레일로 |

동작:
- **PA0 HIGH** → NPN ON → 서보 Signal이 **GND**로 당겨짐 (LOW)
- **PA0 LOW** → NPN OFF → 풀업으로 서보 Signal **5V** (HIGH)

펌웨어: **OCPolarity = LOW** 로 두기. (펄스 구간에 MCU가 LOW 출력 → NPN이 꺼져서 서보선이 5V가 됨.)

```
     +5V
      │
     [R3 10k]
      │
서보 Signal ───┬─────────── 서보 오렌지(신호) 단자
              │
           NPN Collector
           NPN Base ←── [R1 2.2k~4.7k] ←── PA0
           NPN Emitter ──────────────────── GND
```

---

### 방법 2: A1015(PNP)만 쓰기 – 저항 비율로 PNP 끄기 

npn 트랜지스터가 별도로 존재하지 않는다. 

이미 A1015를 쓰고 있다면, Base 저항만 바꿔서 **PA0 HIGH일 때 PNP가 확실히 꺼지게** 할 수 있다.

| 부품 | 연결 |
|------|------|
| **A1015 (PNP)** | Emitter → +5V, Collector → 서보 Signal (+ R3 풀업 to 5V) |
| **Base** | R1 → PA0, **R2 → +5V** (풀업) |

**비율**: PA0이 3.3V일 때 Base가 **5V에 가깝게** 가야 PNP가 꺼진다.  
→ **R1(PA0→Base)을 R2(Base→5V)보다 크게** 잡기.  
예: **R1 = 10kΩ**, **R2 = 4.7kΩ** (Base가 약 4.5V 근처까지 올라가서 PNP OFF).

한계: 이 회로만으로는 서보 신호선을 **GND로 당기는** 구간이 없어서, “펄스 = 5V, 나머지 = 0V” 같은 완전한 PWM이 아니라, 5V만 유지되다가 트랜지스터로 5V를 더 세게 주는 형태가 됨. 서보가 민감하면 동작할 수 있지만, **방법 1 (NPN)** 이 더 안정적이다.

---


# 😭 => 트랜지스터로 변환이 힘들어서 그냥 없이 3.3v로 구동 ㅠㅠㅠㅠ

---

## ⚠️ UART/시리얼 통신이 안 될 때 (VS Code, minicom, screen)

### 1. USART2 RX 인터럽트가 안 들어오는 경우

증상:

- 배너(`50Hz PWM...`)는 보이는데,  
  `mode 0`, `1500` 을 쳐도 **아무 응답(`MODE=...`, `OK ...`)이 안 나옴**.

원인:

- `HAL_UART_Receive_IT()` 는 걸어뒀는데,  
  **NVIC에서 USART2_IRQn 이 Enable 되지 않아서** `HAL_UART_RxCpltCallback()` 이 절대 호출되지 않음.

해결:

1. `Core/Src/stm32f4xx_hal_msp.c` 의 `HAL_UART_MspInit()` 안 `USART2` 케이스에 NVIC 추가:

```c
if(huart->Instance==USART2)
{
  ...
  __HAL_LINKDMA(huart,hdmarx,hdma_usart2_rx);

  /* USART2 interrupt Init: RX 콜백이 불리도록 NVIC 설정 */
  HAL_NVIC_SetPriority(USART2_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(USART2_IRQn);
}
```

2. `Core/Src/stm32f4xx_it.c` 에 실제 IRQ 핸들러 구현:

```c
extern UART_HandleTypeDef huart2;

void USART2_IRQHandler(void)
{
  HAL_UART_IRQHandler(&huart2);
}
```

이 두 가지가 있어야 `USART2` RX → IRQ → `HAL_UART_RxCpltCallback()` 으로 들어온다.

### 2. 콜백은 도는데 `OK ...` 가 안 나오는 경우 (줄 끝 설정)

증상:

- VS Code 시리얼 모니터에서:
  - `---- 전송된 utf8 인코딩 메시지: "1500" ----`
  - 그 아래에 `1500`(에코) 까지는 찍힘
- 하지만, `MODE=0 (manual)` 이나 `OK PA8=1500 PA0=1500 us` 는 전혀 안 나옴.

원인:

- 우리는 RX 콜백에서 **`'\r'` 또는 `'\n'` 을 받았을 때만** 한 줄을 완료:

```c
if (rx_byte == '\r' || rx_byte == '\n')
{
  rx_ready = 1;
}
else if (rx_idx < RX_LINE_MAX - 1)
{
  rx_line_buf[rx_idx++] = (char)rx_byte;
}
```

- VS Code / minicom / screen 에서 **Line ending 이 `None`** 이면  
  `'1'`, `'5'`, `'0'`, `'0'` 만 가고 **`\r`/`\n` 이 안 보내짐** → `rx_ready` 가 절대 1이 안 됨 → `OK ...` 도 안 찍힘.

해결:

- **VS Code 시리얼 모니터**
  - 오른쪽 아래 Line ending 을 **`LF` 또는 `CRLF`** 로 설정.
- **minicom**
  - `Screen and keyboard` 설정에서
    - `P - Add linefeed : Yes`
    - `T - Add carriage return : Yes`
- **screen**
  - 기본적으로 Enter가 `CR` 을 보내므로 별도 설정 없이 동작하는 편.  
    그래도 이상하면 `stty -F /dev/ttyACM0 -echo` 등으로 에코만 조정.

정상일 때 흐름:

1. `'1'`, `'5'`, `'0'`, `'0'`, `'\n'` 순으로 RX 콜백 진입
2. `rx_ready = 1` 이 되고, `while(1)` 루프에서:
   - `sscanf("1500", "%lu %lu", &u1, &u2);` → `u1=1500`, `u2=1500`
   - `__HAL_TIM_SET_COMPARE(..., 1500);`
   - `OK PA8=1500 PA0=1500 us\r\n` 전송

#### 😋값 동작 확인!!

---

## 2026-03-03 – IBVS 제어(X/Y 축 추종) 문제 정리

### 1) 증상 – 타겟 박스 중심을 제대로 따라가지 않던 문제

- **현상 1 (초기 위치 불일치)**  
  - 호스트 `IbvsController` 가 내부적으로 사용하는 시작 PWM 값과  
    STM32 `Servo_Init()` 에서 실제로 설정하는 초기 PWM 값이 서로 달라서,  
    화면 기준으로는 이미 움직인 위치에서 제어가 시작되는 느낌이었음.
- **현상 2 (PAN/TILT 축 뒤바뀜)**  
  - `ibvs_controller.cpp` 에서 PAN/TILT(=X/Y) 초기값이 **서로 뒤집힌 상태**로 들어가 있어서,  
    X축과 Y축이 헷갈려 제어 직관성이 떨어졌음.
- **현상 3 (방향/부호 헷갈림)**  
  - “오른쪽으로 갈수록 PA0 PWM이 증가해야 하고, 아래로 갈수록 PA8 PWM이 감소해야 하는데”  
    실제 움직임이 반대처럼 느껴지거나, 특정 축만 거의 안 움직이는 현상이 있었음.

### 2) 원인 분석

- **PWM 초기값 불일치**
  - STM32 쪽 서보 초기화(`Servo_Init`)에서:
    - `PA8(TIM1_CH1)` ≈ `1250us`
    - `PA0(TIM2_CH1)` ≈ `1430us`
  - 반면, 호스트 IBVS 컨트롤러는 **중심값(중립)을 1500us** 기준으로만 잡고 있어서  
    “호스트 기준 제로”와 “실제 하드웨어 제로”가 맞지 않았다.
- **PAN/TILT 초기값 축 반전**
  - 기존 `IbvsController` 생성자:
    ```cpp
    // ibvs_controller.cpp (5-16)
    IbvsController::IbvsController(int pwm_min_us, int pwm_max_us, int neutral_us,
                                   double Ku, double Kv)
        : pwm_min_(pwm_min_us),
          pwm_max_(pwm_max_us),
          neutral_(neutral_us),
          Ku_(Ku),
          Kv_(Kv),
          // STM32 Servo_Init 과 동일한 초기 PWM 값으로 맞춘다.
          // PA0(TIM2_CH1) ≈ 1430us (PAN, X축)
          // PA8(TIM1_CH1) ≈ 1250us (TILT, Y축)
          current_pan_us_(1430.0),
          current_tilt_us_(1250.0)
    {
    }
    ```
  - 논리적으로는 **PAN(X) = PA0, TILT(Y) = PA8** 로 써야 직관적인데,  
    과거에는 초기값/주석/실제 동작이 섞여 있어서 어디가 X인지 Y인지 사람이 보기 힘든 구조였다.
- **제어 방향(부호) 정리 필요**
  - 요구 사항:
    - X(PAN, PA0): **오른쪽으로 갈수록 PWM 증가**  
      → \( e_u = x_\text{target} - x_\text{laser} \) 가 양수일 때 PA0 us ↑
    - Y(TILT, PA8): **아래로 갈수록 PWM 감소**  
      → \( e_v = y_\text{target} - y_\text{laser} \) 가 양수일 때 PA8 us ↓
  - 따라서 IBVS 게인 부호는:
    - `Ku > 0`
    - `Kv < 0`
  - 코드 상에서 이 부호를 잘못 잡으면 “타겟이 오른쪽인데 왼쪽으로 가는” 식의 반응이 나온다.

### 3) 해결 – PWM 초기값 정렬 + PAN/TILT 축 정리 + 게인 설정

- **(1) STM32 `Servo_Init()` 와 호스트 `IbvsController` 시작값을 일치**
  - STM32 `servo_driver.c`:
    - `PA8(TIM1_CH1) = 1250us`
    - `PA0(TIM2_CH1) = 1430us`
  - 호스트 `IbvsController` 에서도 동일한 값으로 시작하도록 수정:
    - `current_pan_us_ = 1430.0`  (PAN, X축, PA0)
    - `current_tilt_us_ = 1250.0` (TILT, Y축, PA8)
  - 이렇게 해서 **호스트/STM32 양쪽의 “초기 자세” 기준이 완전히 같아지도록 맞춤**.

- **(2) PAN = X = PA0 / TILT = Y = PA8 로 개념 통일**
  - X축(PAN) = PA0(TIM2_CH1), Y축(TILT) = PA8(TIM1_CH1) 으로 정리하고,
  - IBVS 쪽 설명/주석, STM32 `main.c` 의 WiFi 파서 맵핑도 모두 이 기준에 맞춤:
    - `CX → PA0 (TIM2_CH1)`
    - `CY → PA8 (TIM1_CH1)`

- **(3) IBVS 게인/부호 정리**
  - `rtsp_laser_demo.cpp` 에서 컨트롤러 생성 시:
    ```cpp
    // IBVS P 제어기: PWM 범위 800~2200
    // X(파노라마, PA0): Ku > 0  → 오른쪽으로 갈수록 PWM 증가
    // Y(틸트,    PA8): Kv < 0  → 아래로 갈수록 PWM 감소
    IbvsController controller(800, 2200, 1500, 0.02, -0.02);
    ```
  - 에러 계산:
    ```cpp
    double e_u = static_cast<double>(targetROI.center().x - laser.point.x);
    double e_v = static_cast<double>(targetROI.center().y - laser.point.y);
    ```
    - 타겟이 레이저보다 **오른쪽** → `e_u > 0` → `Ku > 0` 이므로 PA0 us 증가 → 오른쪽 회전
    - 타겟이 레이저보다 **아래** → `e_v > 0` → `Kv < 0` 이므로 PA8 us 감소 → 아래 방향으로 기울기 감소

### 4) 결과

- 호스트와 STM32 간 **초기 PWM 레벨이 동일**해져서, 제어 시작점이 일관되게 맞춰졌다.
- PAN/TILT 축 개념을 **PAN(X)=PA0, TILT(Y)=PA8** 로 고정하고,  
  WiFi 파서/IBVS 컨트롤러 모두 이 기준으로 통일하여,  
  “왜 오른쪽으로 드래그했는데 왼쪽으로 움직이냐” 류의 혼란을 줄였다.
- `IbvsController controller(800, 2200, 1500, 0.02, -0.02);` 설정과  
  `e_u/e_v` 정의를 정리한 뒤에는, 바운딩 박스 중심을 기준으로  
  X/Y 축 방향이 **직관적으로 예측 가능한 방향**으로 움직이게 되었다.

---

## 2026-03-03 – WiFi + IBVS 파이프라인 튜닝 (+IPD 다중 라인 처리, 프레임 스로틀링)

### 1) 문제 – +IPD 패킷에 여러 줄이 섞여 들어오면서, 첫 줄만 파싱되던 현상

- **증상**
  - ESP8266 → STM32로 들어오는 WiFi 데이터가 다음과 같이 들어옴:
    - `+IPD,1460:CX=1430...,CY=1252...\r\nCX=1431...,CY=1253...\r\nCX=1432...,CY=1254...\r\n...`
  - 하지만 STM32 `main.c` 에서는:
    ```c
    char *payload = strchr(ipd, ':');
    payload++; /* ':' 뒤부터 실제 데이터 */
    ...
    if (sscanf(payload, "CX=%f,CY=%f", &cx_f, &cy_f) == 2) {
        /* ... Servo_SetAllUs(...) ... */
    }
    ```
    처럼 **payload 전체를 한 번만 `sscanf`** 하고 끝내기 때문에,
    `CX=1430...` **맨 앞 한 줄만 파싱되고** 그 뒤에 이어지는
    `CX=1431...`, `CX=1432...` 등은 모두 무시되는 문제가 있었다.
- **결과**
  - 디버그 로그에는 계속
    - `WiFi PWM CX/CY -> PA8(CY)=1251 PA0(CX)=1430 us`
    같은 값만 반복해서 찍히고,
  - 실제로는 호스트에서 여러 프레임에 걸쳐 보낸 PWM 명령이
    **STM32 쪽에서는 거의 첫 줄 값만 사용되는 것처럼 보이는 현상**이 발생했다.

### 2) 해결 – +IPD payload 를 줄 단위로 쪼개서 모두 파싱

- **변경 내용 (STM32 `main.c`, WiFi +IPD 처리 부분)**
  - `payload` 안에 여러 줄이 섞여 들어올 수 있다는 전제를 두고,
    **줄 단위로 쪼개서 각각 파싱**하도록 수정:
    - 앞쪽 `\r`/`\n` 스킵
    - 한 줄 끝(`\r` 또는 `\n`)까지를 임시로 `'\0'` 로 끊어서 “한 줄”로 간주
    - 그 한 줄에 대해:
      1. 먼저 `CX=%f,CY=%f` 형식 시도
      2. 안 맞으면 `"1500 1400"` / `"1500"` 같은 정수 형식 시도
      3. 둘 중 하나라도 성공하면 즉시 `Servo_SetAllUs(...)` 호출
    - 마지막으로 끊었던 문자를 복원하고, 다음 줄로 넘어감
- **효과**
  - `+IPD,1460:CX=1430...,CY=1252...\r\nCX=1431...,CY=1253...\r\n...` 처럼
    한 번에 여러 줄이 들어와도, **모든 `CX=...,CY=...` 줄이 순서대로 적용**된다.
  - 최종적으로는 가장 마지막 줄의 PWM 명령이 현재 위치를 결정하게 되어,
    호스트에서 보낸 프레임별 제어 명령이 STM32까지 **빠짐없이 전달**된다.

### 3) 문제 – IBVS에서 PWM을 “매 프레임 그대로” 보내면서 서보가 과도하게 자주 갱신되던 현상

- **증상**
  - `rtsp_laser_demo` 는 매 프레임마다 IBVS 제어기를 돌리고,
    매 프레임마다 `stdout` 으로 `PAN_US TILT_US` 를 내보내고 있었다:
    ```cpp
    IbvsOutput out = controller.update(e_u, e_v, dt_sec);
    std::cout << out.pan_us << " " << out.tilt_us << std::endl;
    ```
  - 이 값이 그대로 `ubuntu_tcp_server` → ESP → STM32 로 흘러가면서,
    - 서보에 너무 잦은 업데이트가 들어가고
    - +IPD 패킷 크기도 커지며
    - 전체 파이프라인이 **불필요하게 과잉 업데이트** 되는 형태였다.

### 4) 해결 – `rtsp_laser_demo` 에서 N프레임마다 한 번만 PWM 전송

- **변경 내용 (`host_cpp/src/rtsp_laser_demo.cpp`)**
  - 제어 계산(IBVS 업데이트)은 **매 프레임 유지**하되,
  - 실제로 `stdout`(→ `ubuntu_tcp_server`) 로 내보내는 PWM 명령은
    **N 프레임마다 한 번만 전송**하도록 스로틀링:
    ```cpp
    // Unix 파이프용: stdout에 "PAN_US TILT_US\n" 한 줄만 출력
    // 신호를 너무 자주 보내지 않도록, N프레임마다 한 번씩만 전송
    constexpr int SEND_EVERY_N_FRAMES = 5; // 5~10 프레임 사이에서 튜닝 가능
    if (frame_id % SEND_EVERY_N_FRAMES == 0)
    {
        // 예: 1500 1400
        std::cout << out.pan_us << " " << out.tilt_us << std::endl;
    }
    ```
  - 필요하면 `SEND_EVERY_N_FRAMES` 값을 10, 15 등으로 조절하면서
    실제 모터 반응 속도를 더 천천히 만들 수 있다.

- **효과**
  - 서보에 들어가는 PWM 명령 업데이트 빈도가 줄어들어,
    **움직임이 덜 떨리고 더 “정제된” 느낌**을 준다.
  - 네트워크/ESP/STM32 전체 파이프라인이 불필요하게 과부하되지 않고,
    `+IPD` 버퍼 안에 너무 많은 줄이 한 번에 쌓이는 상황도 완화된다.

