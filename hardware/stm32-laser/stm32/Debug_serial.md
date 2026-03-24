## Debug 시리얼 사용 정리 (PC ↔ STM32 ↔ ESP-8266)

### 1. 기본 환경

- **PC ↔ STM32**
  - 포트: `USART2` (PA2=TX, PA3=RX, ST-LINK VCP)
  - 설정: `115200 8N1`
  - 용도: 디버그 로그, 수동 모드 제어, ESP AT 명령 프록시

- **STM32 ↔ ESP-8266**
  - 포트: `USART1` (PA9=TX, PA10=RX)
  - 설정: `115200 8N1`
  - 용도: AT 명령, TCP 데이터 송수신

---

### 2. 일반 모드 변경 (MANUAL / AUTO)

#### 2.1. 버튼으로 모드 토글

- 보드의 `B1` 버튼(PC13)을 누르면:
  - `MODE_AUTO → MODE_MANUAL`
  - `MODE_MANUAL → MODE_AUTO`
- PC 시리얼 모니터 출력 예:

```text
manual mode 실행
auto mode 실행
```

#### 2.2. 레이저 ON/OFF 제어 (PB0)

- 레이저 제어 신호: `PB0` (`GPIOB pin 0`)
- 펌웨어 동작(현재 구현):
  - `B1` 버튼(PC13)을 누를 때마다 `PB0`가 `H ↔ L` 로 **토글**됩니다.
  - 따라서 `PB0`에 연결된 레이저 구동 회로(릴레이/트랜지스터 등)에 따라 레이저가 `ON/OFF`로 전환됩니다.
- 회로 극성:
  - 사용 중인 모듈이 "GPIO HIGH면 레이저 ON / GPIO LOW면 레이저 OFF" 형태인지 확인해주세요. (모듈에 따라 반대일 수 있습니다.)
- 확인 방법:
  - 펌웨어 플래시 후 `B1`를 연속으로 눌러 `PB0(GPIOB pin0)` 전압(멀티미터/테스트 LED)을 보면 `H↔L` 토글이 관측됩니다.

#### 2.3. 시리얼 명령으로 모드 변경

- PC 시리얼에서 한 줄로:

```text
mode 0
mode 1
```

- 의미:
  - `mode 0` → **수동(MANUAL)** 모드
  - `mode 1` → **자동(AUTO)** 스윕 모드 (기본 1200~1800us 왕복)

- 응답 예:

```text
MODE=0 (manual)
MODE=1 (auto sweep 1200~1800us)
```

---

### 3. 회전각(서보 위치) 변경

PWM 펄스폭(us)을 직접 지정해서 서보 각도를 제어한다.

#### 3.1. 두 채널 동일 값 설정

- PC 시리얼:

```text
1500
```

- 의미:
  - CH1=PA8(TIM1_CH1), CH2=PA0(TIM2_CH1)를 **둘 다 1500us** 로 설정.

#### 3.2. 두 채널 각각 설정

- PC 시리얼:

```text
1500 1200
```

- 의미:
  - CH1=PA8 = 1500us
  - CH2=PA0 = 1200us

#### 3.3. 개별 채널(X/Y) 설정

- **X 채널만 변경 (PA0 / TIM2_CH1)**:

  ```text
  X:1500
  X 1500
  ```

  - 의미: PA0(TIM2_CH1)만 1500us 로 설정 (PA8은 기존 값 유지)
  - 응답:

    ```text
    OK X=PA0=1500 us
    ```

- **Y 채널만 변경 (PA8 / TIM1_CH1)**:

  ```text
  Y:1200
  Y 1200
  ```

  - 의미: PA8(TIM1_CH1)만 1200us 로 설정 (PA0은 기존 값 유지)
  - 응답:

    ```text
    OK Y=PA8=1200 us
    ```

#### 3.4. 응답 예

```text
OK PA8=1500 PA0=1500 us
OK PA8=1500 PA0=1200 us
OK X=PA0=1500 us
OK Y=PA8=1200 us
```

- 잘못된 형식일 경우:

```text
? (send: 1500 or 1500 1200, X:1500, Y:1500, or mode 0/1)
```

---

### 4. ESP-8266 AT 명령 제어 (PC에서 직접 ESP 터미널처럼 사용)

PC 시리얼(USART2)에서 ESP로 AT 명령을 보내는 방법은 두 가지가 있다.

#### 4.1. 직접 AT로 시작하는 라인

- PC 시리얼:

```text
AT
AT+RST
AT+CIPSTATUS
AT+CWMODE=1
```

- 동작 팁:
  - 시리얼 모니터에서 `AT...` 입력 후 반드시 `Enter`(CR/LF)가 함께 전송되게 설정하세요.
  - (TCP 연결 전이라도) STM32가 AT 패스스루 직후 일정 시간 동안 ESP 응답을 에코해줍니다.

- 동작:
  - STM32가 라인이 `AT` 또는 `at` 로 시작하면,
  - 내용을 그대로 `USART1(ESP)` 로 전달:

    ```c
    Wifi_SendLine("AT+RST");  // "AT+RST\r\n" 이 ESP로 전송
    ```

  - ESP 응답(`OK`, `ERROR`, `CLOSED`, `ALREADY CONNECTED`, `+IPD,...` 등)은
    모두 **PC 시리얼(USART2)** 에 그대로 에코된다.

#### 4.2. `ESP:` 프리픽스 사용 (권장)

- PC 시리얼:

```text
ESP: AT
ESP: AT+RST
ESP: AT+CIPSTATUS
ESP: AT+CWMODE=1
ESP:   AT+CIPMODE=0
```

- 동작:
  1. STM32가 `ESP:` 또는 `esp:` 로 시작하는 라인을 받으면,
  2. `ESP:` 프리픽스를 떼고 뒤의 문자열만 추출 (`AT+RST` 등),
  3. 앞뒤 공백 제거 후 `Wifi_SendLine()` 으로 ESP(USART1)에 전송 (자동으로 `\r\n` 첨부),
  4. PC 시리얼에는 디버그용으로 다음처럼 에코:

     ```text
     ESP:AT+RST
     ```

- 장점:
  - ESP 관련 명령과 일반 모드/서보 제어 명령을 **시각적으로 구분**하기 쉬움.
  - 나중에 필요하면 `ESP:` 프리픽스만 따로 필터링해서 로그 볼 수도 있음.

---

### 5. PING/PONG RTT 테스트 (요약)

- **라즈베리 Pi 서버**에서 `PING,<seq>,<t0_ms>\n` 형식으로 TCP 데이터 전송.
- ESP-8266 → STM32 경로:
  - `+IPD,...:PING,<seq>,<t0_ms>` 형태 한 줄을 DMA/IDLE로 수신.
  - STM32는 문자열 안에서 `"PING,"` 서브스트링을 찾아서,

    ```text
    PONG,<seq>,<t0_ms>\n
    ```

    을 만들고 **AT+CIPSEND → '>' 프롬프트 → PONG 전송** 순으로 즉시 응답.
- Pi 쪽에서는 `PONG,...` 수신 시점까지의 RTT를 측정한다.

이 테스트 과정에서 ESP의 AT 응답(`+IPD`, `SEND OK`, `busy p...` 등)은 전부 PC 시리얼에 보이므로,  
