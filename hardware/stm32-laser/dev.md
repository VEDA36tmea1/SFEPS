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
 