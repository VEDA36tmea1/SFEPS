## STM32 Laser (Galvo) Firmware - Code Structure & Protocol Notes

Nucleo STM32F401RE 펌웨어는 다음을 동시에 수행합니다.

1. 서보/갈보 PWM 2채널(TIM1/TIM2)을 구동(`PWM 800~2200 us`, 50Hz 기본)
2. 레이저 Enable을 `PB0`로 ON/OFF
3. `ESP8266`(USART1)와 통신하여 TCP payload(`+IPD,...:`)를 수신하고 파싱
4. `USART2`(PC)에서 들어오는 입력을 파싱(AT pass-through, mode 전환, PWM 설정 등)
5. `TRACK_START / TRACK_END` 수신 시 서버로 ACK를 다시 TCP로 전송

---

## 1) 핵심 디렉터리/파일

### 펌웨어 진입점
- `Core/Src/main.c`
  - 초기화: TIM/PWM, USART2/USART1, DMA/IDLE 수신, LED, Parser 콜백 세팅
  - 메인 루프:
    - 버튼(B1) 처리
    - USART2 라인 수신(rx_ready) → `UART_Parser_HandleLine()`
    - ESP(+IPD) 수신(wifi_rx_pending) → `ESP_Parser_HandleIpdLine()` 우선 처리 후 나머지 legacy 처리
    - PING/PONG RTT 처리 및 `Wifi_MaybeReconnect()`

### 파서 분리
- `Core/Inc/ESP_Parser.h`, `Core/Src/ESP_Parser.c`
  - `+IPD` payload 내부의 `TRACK_START / TRACK_POS / TRACK_END` 파싱
  - PB0 레이저 on/off 및 서버 ACK 전송 큐잉

- `Core/Inc/UART_Parser.h`, `Core/Src/UART_Parser.c`
  - `USART2`(PC)에서 들어오는 1라인을 파싱
  - `AT...` / `ESP: ...` pass-through
  - `mode 0/1` 및 PWM 명령(X/Y/정수/2개 값)

### PWM/LED 드라이버
- `Core/Inc/servo_driver.h`, `Core/Src/servo_driver.c`
  - TIM1/TIM2 CH1에 PWM 설정
- `Core/Inc/led_driver.h`, `Core/Src/led_driver.c`
  - 자동(AUTO) 모드이면 보드 LED LD2를 켬

---

## 2) 통신 구조(ESP / PC)

### 2.1 PC ↔ STM32 (USART2)
- PC 터미널/시리얼 모니터에서 들어오는 라인을 `USART2`로 수신
- `rx_ready`가 되면 `UART_Parser_HandleLine(line_ptr)`을 호출

지원 입력(요약)
- `AT...` : 그대로 ESP로 전송(ESP 응답 에코는 `at_echo_until_tick`으로 제한)
- `ESP: ...` : `ESP:` 제거 후 그대로 ESP로 전송
- `mode 0` : 수동(MANUAL)
- `mode 1` : 자동(AUTO, 기본 1200~1800 us 스윕)
- `1500` : X/Y 둘 다 1500us
- `1500 1200` : (기존 규칙) PA8(Y)=1500, PA0(X)=1200 형태로 설정
- `X:1500` : PA0(TIM2_CH1)만 변경
- `Y:1200` : PA8(TIM1_CH1)만 변경

### 2.2 STM32 ↔ ESP8266 (USART1 + DMA/IDLE)
- `USART1`에서 들어오는 데이터를 DMA로 받고, `UART_IT_IDLE` 발생 시 한 줄(`+IPD,...:` 포함) 단위 처리합니다.
- `main.c`에서 `wifi_rx_pending`이 뜨면 `wifi_line`을 만든 뒤:
  1. `+IPD` 내부 payload를 `\r\n` 기준으로 “한 줄씩” 쪼갬
  2. 각 줄에 대해 우선 `ESP_Parser_HandleIpdLine(cursor)` 실행
  3. 트랙 명령을 처리하면 `continue`로 legacy 처리( EX/CX/PWM )를 스킵
  4. 트랙을 처리하지 않는 라인은 기존 legacy 파서(EX/CX/PWM/숫자 등)로 처리

---

## 3) +IPD 수신 파싱: legacy + TRACK

### 3.1 라인 분리 방식
`main.c`에서 `payload` 문자열 내부를 다음처럼 처리합니다.
- `+IPD` 이후 콜론(`:`) 뒤부터 payload 시작
- payload 끝의 `\r`/`\n` 제거
- 그 다음 `\r\n`으로 “한 줄(cursor)”을 만들고, cursor마다 다음을 수행:
  - `ESP_Parser_HandleIpdLine(cursor)` (TRACK_* 처리용)
  - 실패하면 legacy:
    - `EX=...,EY=...` : IBVS PID 입력
    - `CX=...,CY=...` : 기존 직접 PWM 경로(예: `Servo_SetAllUs`)
    - `1500 1400` 또는 `1500` : 서보 PWM 직접 설정

### 3.2 WiFi/TCP 상태 파싱(연결 관리)
`main.c`는 ESP 응답 문자열에서 아래 패턴을 찾아 연결 상태를 업데이트합니다.
- 연결 성공으로 판정:
  - `CONNECT`, `ALREADY CONNECTED`, `STATUS:3`, `+IPD`, `SEND OK`, `OK` 등 포함 시
- 끊김으로 판정:
  - `CLOSED`, `STATUS:4` 등 포함 시
- 에러/실패는 `ERROR`/`FAIL`을 찾아 송신 pending 등을 정리

> 주의: `WIFI_SERVER_IP`/`WIFI_SERVER_PORT`는 현재 코드에 상수로 들어있습니다. 서버 포트와 반드시 맞춰야 합니다.

---

## 4) 서버 응답(ACK) 전송 방식

### 4.1 TRACK ACK를 보내는 경로
- `ESP_Parser.c`에서 `TRACK_START`/`TRACK_END`를 인식하면:
  1. 레이저(PB0) on/off 실행
  2. `tcp_send` 콜백을 통해 ACK payload를 큐잉
  3. `main.c`에서 `wifi_send_pending` + ESP의 `>` 프롬프트 수신 시 실제 TCP payload로 전송

### 4.2 현재 TRACK ACK 포맷
- `TRACK_START_ACK|<object_id>\n`
- `TRACK_END_ACK|<object_id>|REASON=<reason>\n` (reason 존재 시)
- `TRACK_END_ACK|<object_id>\n` (reason 미존재 시)

> 서버가 요구하는 ACK 포맷이 다르면, `ESP_Parser.c`의 `snprintf(resp, ...)` 부분만 맞추면 됩니다.

### 4.3 PC 시리얼 모니터(USART2)에서 TRACK 확인
- **이전 동작**: `ESP_Parser`는 ACK만 TCP로 보내고, **START/END 전용 USART2 한 줄 로그는 없었음**. WiFi(USART1) 원문은 `main.c`에서 TCP 연결 후(`wifi_link_ok`)에만 PC로 에코됨.
- **현재**: `esp_parser_callbacks_t.dbg_tx`에 `UART_TxPc`를 연결해, `TRACK_START` / `TRACK_END` 처리 시 다음 형태의 줄이 **항상** USART2로 나감:
  - `[TRACK] START id=... (PB0 ON if not manual)\r\n`
  - `[TRACK] END id=... REASON=... (PB0 OFF if not manual)\r\n` (또는 reason 없을 때 REASON 없이)
- `TRACK_POS`는 초당 수십 번 호출될 수 있어 **디버그 한 줄은 출력하지 않음** (원문은 위 `wifi_link_ok` 에코로 확인).

---

## 5) 레이저 조작 (PB0)

### 5.1 핀 정의
- `PB0` (`GPIOB pin 0`) : 레이저 Enable 출력
- `PC13` (`B1` 버튼) : 수동/자동 모드 토글용 입력(동시에 PB0 수동 제어도 수행)

### 5.2 TRACK 기반 레이저 제어
- `TRACK_START|<object_id>` 수신 시: `PB0 = HIGH` (레이저 ON)
- `TRACK_END|<object_id>|REASON=...` 수신 시: `PB0 = LOW` (레이저 OFF)
- `TRACK_POS`는 현재 단계에서는 ACK/서보 구동만 하지 않고 “인식만” 처리 후 무시

### 5.3 B1 버튼 기반 레이저 수동 제어
- `B1` 누르면:
  - MODE_AUTO ↔ MODE_MANUAL 토글
  - `PB0`도 토글
  - 이후 `laser_manual_override` 플래그를 세워서, ESP의 TRACK_START/END가 PB0를 덮어쓰지 못하게 구성되어 있습니다.

> 결과적으로, “B1으로 레이저를 원격/서버와 상관없이 계속 제어”하려면 이 오버라이드 로직이 동작해야 합니다.
> 반대로 “트랙 제어가 다시 원복”되길 원하면, 어떤 조건에서 오버라이드를 해제할지(예: TRACK_END 시 해제, 특정 UART 커맨드로 해제 등) 규칙을 정해야 합니다.

---

## 6) PWM 제어(서보/갈보)

### 6.1 타이머/채널 매핑
- `TIM1` → `PA8` → CH1
  - `Servo_SetCh1Us(us)` / `servo_driver`의 CH1 업데이트
- `TIM2` → `PA0` → CH1
  - `Servo_SetCh2Us(us)` / `servo_driver`의 CH2 업데이트

`Servo_Init()` 기본값
- `TIM1(PA8)`: 1250us
- `TIM2(PA0)`: 1430us

### 6.2 범위/주기
- 주파수: `PWM_FREQ_HZ = 50` (주기 20ms)
- 펄스 범위: `PWM_US_MIN = 800`, `PWM_US_MAX = 2200`
- 모든 PWM 설정은 드라이버에서 범위 클램프 후 반영됩니다.

### 6.3 명령 입력 규칙(USART2)
- `UART_Parser`가 아래 명령을 지원합니다.
  - `1500` / `1500 1200` / `X:1500` / `Y:1200`
  - `mode 0/1`로 AUTO 스윕이 동작/정지합니다.

---

## 7) 핀 요약 테이블

| 용도 | STM32 핀 | 동작 방향 | 파일/함수 |
|---|---|---|---|
| B1 버튼 | `PC13` (`B1_Pin`) | 입력(EXTI Falling) | `main.c`, `HAL_GPIO_EXTI_Callback()` |
| 레이저 Enable | `PB0` (`GPIOB pin 0`) | 출력 | `main.c`(GPIO init), `PB0_SetLaser()`, `ESP_Parser.c` |
| PWM CH1 (Y/TILT) | `PA8` (TIM1_CH1) | 출력(PWM) | `servo_driver.c`: `Servo_SetCh1Us()` |
| PWM CH1 (X/PAN) | `PA0` (TIM2_CH1) | 출력(PWM) | `servo_driver.c`: `Servo_SetCh2Us()` |
| AUTO LED 표시 | `PA5` (`LD2_Pin`) | 출력 | `led_driver.c`: `Led_SetAutoMode()` |

추가 통신 핀(참고)
- `USART1`(ESP) : `PA9=TX`, `PA10=RX` (STM32 ↔ ESP8266)
- `USART2`(PC) : ST-LINK VCP 사용(일반적으로 `PA2=TX`, `PA3=RX`)

---

## 8) 현재 코드에서의 TRACK 명령 포맷(수신 기준)

서버가 STM32로 보내는 메시지는 `+IPD` payload 내부 “한 줄”로 들어온다고 가정합니다.

- 시작: `TRACK_START|<object_id>`
- 위치 업데이트: `TRACK_POS|<object_id>|L=...|T=...|R=...|B=...|X=...|Y=...|CX=...|CY=...|W=...|H=...`
- 종료: `TRACK_END|<object_id>|REASON=<...>`

---

## 9) TODO / 주의점

1. `TRACK_POS`는 현재 단계에서 별도 ACK/서보 업데이트 없이 무시합니다(“서보/좌표 추종 로직을 확장”하면 여기서 처리).
2. 레이저 on/off의 “HIGH=ON” 여부는 사용 중인 레이저 드라이버/회로 극성에 따라 반전될 수 있습니다. 필요하면 `PB0_SetLaser(on)`에서 on/off만 뒤집으면 됩니다.
3. 서버 포트 불일치(예: 서버는 5555, 펌웨어는 5565)면 재연결만 반복됩니다. `WIFI_SERVER_PORT`와 서버 포트를 맞추세요.

