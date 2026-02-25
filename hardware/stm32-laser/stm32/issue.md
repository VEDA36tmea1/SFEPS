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

