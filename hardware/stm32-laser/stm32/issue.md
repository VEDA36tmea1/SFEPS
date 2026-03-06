## ESP-8266 TCP 재연결 & 응답 미출력 이슈 정리

### 1. 증상 요약

- **PC 시리얼 모니터**
  - 보드 리셋 후 `50Hz PWM...` 안내 문구는 보이는데,
  - `AT+CIPSTART="TCP","192.168.4.1",5555` 이후 ESP의 `OK`, `CONNECT`, `ALREADY CONNECTED` 등의 응답이 전혀 보이지 않음.
  - `WiFi: try reconnect TCP` + `AT+CIPSTART=...` 로그가 **계속 반복**됨.
- **라즈베리 `raspi_tcp_server`**
  - `CX=0.300000,CY=0.300000` 같은 로그가 뜨는 것으로 보아 **ESP ↔ 서버 TCP 연결 자체는 살아 있음**.

### 2. 원인 1 – USART1 DMA/IDLE 인터럽트 핸들러 누락

- `stm32f4xx_it.c` 에 **USART1(ESP-8266) 관련 핸들러가 전혀 없었음**:
  - `USART1_IRQHandler` 부재
  - `DMA2_Stream2_IRQHandler` 부재
- `main.c` 에서는 `wifi_rx_pending` / `wifi_rx_len` 플래그를 보고
  `if (wifi_rx_pending) { ... }` 블록에서 ESP 응답을 PC 시리얼로 에코하도록 되어 있었지만,
  - 인터럽트가 `wifi_rx_pending = 1` 을 **한 번도 세우지 않아서** 이 블록이 실행되지 않았음.

#### 해결

1. **전역/extern 정리**

   - `Core/Src/main.c`:

     ```c
     UART_HandleTypeDef huart1;
     …
     DMA_HandleTypeDef hdma_usart1_rx;
     DMA_HandleTypeDef hdma_usart2_rx;
     ```

   - `Core/Src/stm32f4xx_hal_msp.c` 상단:

     ```c
     extern DMA_HandleTypeDef hdma_usart2_rx;
     extern DMA_HandleTypeDef hdma_usart1_rx;
     ```

   - `Core/Src/stm32f4xx_it.c` 상단:

     ```c
     extern DMA_HandleTypeDef hdma_usart2_rx;
     extern UART_HandleTypeDef huart2;

     extern DMA_HandleTypeDef hdma_usart1_rx;
     extern UART_HandleTypeDef huart1;
     extern volatile uint8_t  wifi_rx_pending;
     extern volatile uint16_t wifi_rx_len;
     #define WIFI_RX_DMA_SIZE 256
     ```

2. **USART1 RX용 DMA 및 NVIC 설정 추가** – `HAL_UART_MspInit(USART1)`:

   ```c
   __HAL_RCC_USART1_CLK_ENABLE();
   __HAL_RCC_GPIOA_CLK_ENABLE();
   // PA9/PA10 AF7 설정 (기존 동일)

   __HAL_RCC_DMA2_CLK_ENABLE();

   hdma_usart1_rx.Instance = DMA2_Stream2;
   hdma_usart1_rx.Init.Channel = DMA_CHANNEL_4;
   hdma_usart1_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
   hdma_usart1_rx.Init.PeriphInc = DMA_PINC_DISABLE;
   hdma_usart1_rx.Init.MemInc = DMA_MINC_ENABLE;
   hdma_usart1_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
   hdma_usart1_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
   hdma_usart1_rx.Init.Mode = DMA_NORMAL;
   hdma_usart1_rx.Init.Priority = DMA_PRIORITY_HIGH;
   hdma_usart1_rx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
   HAL_DMA_Init(&hdma_usart1_rx);
   __HAL_LINKDMA(huart, hdmarx, hdma_usart1_rx);

   HAL_NVIC_SetPriority(USART1_IRQn, 0, 0);
   HAL_NVIC_EnableIRQ(USART1_IRQn);
   ```

3. **USART1 / DMA2 인터럽트 핸들러 구현** – `stm32f4xx_it.c`:

   ```c
   void DMA2_Stream2_IRQHandler(void)
   {
     HAL_DMA_IRQHandler(&hdma_usart1_rx);
   }

   void USART1_IRQHandler(void)
   {
     if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_IDLE))
     {
       __HAL_UART_CLEAR_IDLEFLAG(&huart1);
       uint16_t n = (uint16_t)__HAL_DMA_GET_COUNTER(huart1.hdmarx);
       wifi_rx_len = WIFI_RX_DMA_SIZE - n;
       (void)HAL_UART_AbortReceive(&huart1);
       wifi_rx_pending = 1;
     }
     HAL_UART_IRQHandler(&huart1);
   }
   ```

4. **결과**

- ESP가 `OK`, `CONNECT`, `ALREADY CONNECTED`, `+IPD ...` 등을 보내면
  - `wifi_rx_pending == 1` 이 되고,
  - `main()` 루프의 `if (wifi_rx_pending)` 블록이 실행되면서
    **ESP 응답이 그대로 PC 시리얼(USART2)로 에코**됨.

---

### 3. 원인 2 – WiFi/TCP 상태 플래그(`wifi_link_ok`) 갱신 로직 문제

- `Wifi_MaybeReconnect()`:

  ```c
  if (wifi_link_ok || wifi_connecting)
    return;
  // 아니면 5초마다 AT+CIPSTART("TCP","192.168.4.1",5555) 전송
  ```

- 하지만 `wifi_link_ok` 를 1로 바꾸는 로직이 약해서,
  - ESP 응답이 다음처럼 들어올 때:

    ```text
    ALREADY CONNECTED
    ERROR
    ```

  - `ALREADY CONNECTED` 에서 잠깐 1이 되더라도,
  - 바로 이어지는 `ERROR` 를 “실패”로 간주해 다시 0으로 내려버림.
- 결과:
  - 실제로는 이미 서버에 잘 붙어 있는데도,
  - 펌웨어 입장에서는 **계속 끊긴 상태(wifi_link_ok == 0)라고 믿고**  
    `WiFi: try reconnect TCP` + `AT+CIPSTART=...` 을 반복해서 찍음.

#### 해결 – 응답 문자열 기반 상태 머신 강화

`wifi_rx_pending` 처리에서 한 줄(`wifi_line`) 기준으로 아래 규칙을 적용:

```c
/* 1) 명확한 끊김 패턴 - CLOSED / STATUS:4 는 항상 연결 끊김으로 간주 */
if (strstr(wifi_line, "CLOSED") != NULL ||
    strstr(wifi_line, "STATUS:4") != NULL)
{
  wifi_link_ok    = 0;
  wifi_connecting = 0;
}
/* 2) 명확한 성공/연결 패턴들을 먼저 처리
 *    (한 줄에 OK + ERROR 같이 있어도 "성공 우선") */
else if (strstr(wifi_line, "ALREADY CONNECTED") != NULL ||
         strstr(wifi_line, "CONNECT") != NULL ||
         strstr(wifi_line, "STATUS:3") != NULL ||
         strstr(wifi_line, "+IPD") != NULL ||
         strstr(wifi_line, "SEND OK") != NULL ||
         strstr(wifi_line, "OK") != NULL)
{
  wifi_link_ok    = 1;
  wifi_connecting = 0;
}
/* 3) ERROR / FAIL
 *  - 아직 연결 안 된 상태(wifi_link_ok == 0)에서 나오면 "연결 실패"로만 처리
 *  - 이미 연결 OK 상태라면(예: ALREADY CONNECTED 이후) 단순 재요청 실패로 보고 연결은 유지 */
else if (strstr(wifi_line, "ERROR") != NULL ||
         strstr(wifi_line, "FAIL")  != NULL)
{
  /* 연결 OK 상태면 wifi_link_ok 유지, 진행 중이던 연결 시도만 중단 */
  wifi_connecting = 0;
}
```

#### 결과 동작

- **처음 연결 시도**
  - `AT+CIPSTART=...` → ESP 응답에 `OK` / `CONNECT` / `STATUS:3` / `ALREADY CONNECTED` / `+IPD` / `SEND OK` 중 하나라도 포함되면:
    - `wifi_link_ok = 1`, `wifi_connecting = 0`
    - 이후 `Wifi_MaybeReconnect()` 는 **재시도하지 않음.**

- **이미 연결된 상태에서 다시 CIPSTART를 보낸 경우**
  - ESP 출력:

    ```text
    ALREADY CONNECTED
    ERROR
    ```

  - `ALREADY CONNECTED` / `OK` 등에서 `wifi_link_ok = 1` 로 유지,
  - 뒤이어 오는 `ERROR` 는 “중복 요청 실패”로만 처리 (`wifi_connecting = 0` 만).
  - **TCP 연결은 유지되고, 재접속 루프에 빠지지 않음.**

- **서버가 꺼져서 실제로 끊긴 경우**
  - ESP 응답에 `CLOSED` / `STATUS:4` 가 나오면:
    - `wifi_link_ok = 0`, `wifi_connecting = 0`
    - 이후 5초마다 `AT+CIPSTART=...` 재시도 → 서버가 다시 뜨면 자동으로 재연결.

---

### 4. 요약

- **문제 1**: USART1(DMA/IDLE) 인터럽트 미구현 → ESP 응답이 `main()` 에 전달되지 않음.
- **문제 2**: WiFi/TCP 상태 플래그(`wifi_link_ok`) 갱신 로직이 단순해서,
  - `ALREADY CONNECTED` + `ERROR` 패턴을 “끊김”으로 착각 → 무한 재시도.
- **해결**:
  - `stm32f4xx_it.c` / `stm32f4xx_hal_msp.c` / `main.c` 에 USART1 DMA/IDLE 경로 완전히 연결.
  - ESP 응답 문자열 기반으로 `wifi_link_ok` / `wifi_connecting` 을 세밀하게 갱신하는 상태 머신 추가.

이후에는:

- PC 시리얼 모니터에서 **ESP AT 응답이 그대로 보이고**,  
- 한 번 `ALREADY CONNECTED` / `OK` / `STATUS:3` / `+IPD` 가 찍힌 뒤에는  
  **TCP 연결이 실제로 끊기기 전까지는 `CIPSTART` 재시도를 더 이상 하지 않는다.**

---
---

## 2026-02-26 – ST-LINK 인식 실패 (SWD 연결 & stlink-tools 빌드 이슈)

### 1. 증상 요약

- 라즈베리 파이에서 `st-flash` / `st-info --probe` 실행 시:
  - `Failed to enter SWD mode`
  - `flash: 0 (pagesize: 0)`, `sram: 0`, `chipid: 0x000`, `dev-type: unknown`
- `lsusb` 에서는 `STMicroelectronics ST-LINK/V2.1` 가 정상적으로 보임.
- 즉, **USB로 ST-LINK는 잡히는데, 타깃 STM32F401RE MCU와의 SWD 통신이 전혀 안 되는 상태**.

### 2. 원인 1 – STM32를 브레드보드에 꽂은 상태에서 Nucleo ST-LINK만 사용

- 실제로는 **Nucleo 보드의 ST-LINK (ST-LINK/V2.1)** 만 라즈베리 파이에 USB로 연결해두고,
  STM32 칩 자체는 별도의 **브레드보드 위에 장착된 상태**였음.
- 이 과정에서:
  - 브레드보드 쪽 회로/배선이 **SWDIO / SWCLK / RESET / 전원 라인에 영향을 주고 있었고**,  
  - 결과적으로 **SWD 라인이 깨끗하게 분리되지 않아 ST-LINK가 타깃과 핸드셰이크를 못 함**.
- Nucleo 온보드 MCU만 사용할 때보다 배선/접촉 포인트가 늘어나면서,
  ST-LINK 쪽에서는 항상 "프로그램머는 있음"으로 보이지만,
  실제 MCU 정보는 전혀 읽어오지 못하는 상태가 됨.

#### 해결

- STM32를 브레드보드에서 제거하고, **Nucleo 온보드 MCU만 사용**한 상태에서 다시 테스트:
  - `st-info --probe` 에서 정상적으로 flash/sram/chipid 가 읽힘.
  - 이후 `st-flash write stm32_laser.bin 0x8000000` 가 정상 동작.
- 결론:
  - **SWDIO/SWCLK/RESET/전원 라인은 매우 민감**하므로,
    브레드보드/외부 회로에 물려둘 경우 반드시 **전기적 상태(풀업/풀다운, 부하, 쇼트 가능성)를 먼저 확인**해야 한다.

### 3. 원인 2 – stlink-tools(1.8.0) 빌드 시 의존성/CMake 환경 문제

라즈베리 파이에서 최신 `stlink`(1.8.0)를 소스 빌드할 때, 다음 이슈들이 순차적으로 발생했다.

1. **CMake 3.18 + C17 요구**
   - 기본 CMake 3.18은 C17(C_STANDARD 17)에 대한 컴파일 플래그 정보를 갖고 있지 않아,
     `Target ... requires the language dialect "C17" ... but CMake does not know the compile flags to use to enable it.` 에러 발생.
   - `CMakeLists.txt` 에서 C 표준을 C11로 낮추거나
     (`set(CMAKE_C_STANDARD 11)` + 필요 시 버전 분기) 로 해결.

2. **libusb 개발 패키지 누락**
   - `Could NOT find libusb (missing: LIBUSB_LIBRARY)` 에러.
   - 해결:
     - `sudo apt install libusb-1.0-0-dev pkg-config`
     - 경로가 특이한 경우, CMake 호출 시
       `-DLIBUSB_INCLUDE_DIR=/usr/include/libusb-1.0`
       `-DLIBUSB_LIBRARY=/usr/lib/aarch64-linux-gnu/libusb-1.0.so`
       를 명시적으로 지정.

3. **설치 후 PATH/캐시 문제**
   - `sudo make install` 이 `/usr/local/bin/st-flash` 에 설치됐는데도
     `bash: /usr/bin/st-flash: No such file or directory` 가 뜸.
   - 원인: bash가 예전 `/usr/bin/st-flash` 경로를 캐시하고 있었음.
   - 해결: 새 터미널을 열거나 `hash -r` 로 명령 경로 캐시를 비운 뒤 다시 실행.

### 4. 요약

- **하드웨어**:
  - 브레드보드에 STM32를 꽂은 상태 + Nucleo ST-LINK만 사용하면
    SWD 라인이 외부 회로/배선에 의해 깨져 `Failed to enter SWD mode` 가 뜰 수 있다.
  - 가능하면 **Nucleo 온보드 MCU만 단독으로 먼저 플래시/디버그가 되는지 확인**한 뒤,
    이후에 외부 회로로 확장하는 것이 안전하다.
- **소프트웨어/툴체인**:
  - 라즈베리 파이 기본 CMake(3.18)는 C17 지원이 부족 → stlink 빌드 시 C 표준을 C11로 낮춰 해결.
  - `libusb-1.0-0-dev`, `pkg-config` 설치 및 필요 시 `LIBUSB_INCLUDE_DIR`, `LIBUSB_LIBRARY` 수동 지정으로 CMake 단계 통과.

---
## 2026-02-24 - 모터 노이즈 문제 해결

#### 1. 증상

- 서보가 중립(예: 1500us)에서도 `지지직` 소음을 내며 떨림.
- Servo tester에서는 정상 동작하고, STM32 PWM 출력에서만 이상 증상 발생.

#### 2. 원인 분석

- TIM PWM 채널 설정에서 출력 극성이 반대로 설정되어 있었음.
- 문제 설정:

  ```c
  sConfigOC.OCPolarity = TIM_OCPOLARITY_LOW;
  sConfigOC.OCNPolarity = TIM_OCNPOLARITY_HIGH;
  ```

- 위 상태에서는 PWM이 반전되어, 예를 들어 1500us 명령이 실제로는
  거의 전체 주기 High에 가까운 형태로 전달됨.
- 그 결과 서보 내부 제어 기준에서 비정상 펄스로 해석되어
  엔드스톱 방향으로 과도하게 힘을 주면서 소음/발열/진동이 유발됨.

#### 3. 수정 내용

- TIM1/TIM2 PWM 채널의 극성을 서1보 일반 규격에 맞게 정방향으로 조정:

  ```c
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCNPolarity = TIM_OCNPOLARITY_HIGH;
  ```

- 50Hz(20ms) 기준에서 1000~2000us 펄스 폭이 정상 의미로 전달되도록 정렬.

#### 4. 결과

- 중립 및 수동 입력 구간에서 비정상 소음이 크게 감소.
- AUTO 스윕/수동 명령 모두에서 목표 각도 추종 안정성 개선.

#### 5. 체크 포인트

- 동일 이슈 재발 방지를 위해 TIM 채널 생성/재생성(CubeMX) 후
  `MX_TIM1_Init`, `MX_TIM2_Init`의 `OCPolarity`를 반드시 재검토.
- PWM 관련 회귀 테스트 시 아래 항목 포함:
  - 1500us 고정 시 소음/진동 확인
  - 1200/1500/1800us 스텝 입력 시 응답 확인
  - 50Hz 주기 및 극성(High-active) 확인

