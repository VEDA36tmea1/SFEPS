# Ubuntu 서버 + 노트북 AP 구성 가이드

이 디렉터리는 **라즈베리 파이 대신 Ubuntu 노트북**에서 TCP 서버를 띄우고,
ESP8266/STM32가 노트북에 직접 접속하도록 구성할 때 사용하는 정리용 문서입니다.

- 라즈베리 파이용 코드는 `../raspi_server/` 참조
- 여기서는 **노트북을 Wi‑Fi AP(핫스팟)** 로 만들고, `raspi_tcp_server`와 동일한 서버를
  노트북에서 실행하는 시나리오를 다룹니다.

---

## 1. 네트워크 / 통신 개요

```text
[ESP8266] <-> UART <-> [STM32] <-> (Wi‑Fi) <-> [Ubuntu 노트북(AP + TCP 서버)]
```

- **Ubuntu 노트북**: Wi‑Fi 인터페이스로 AP(핫스팟)를 켠 뒤, 해당 인터페이스 IP에서 TCP 서버 실행
- **ESP8266**: 노트북 AP의 SSID/비번으로 연결 → 노트북 IP, 포트(예: 5555)로 TCP 접속
- **STM32**: ESP8266과 UART로 연결되어, 수신한 좌표/명령을 파싱하여 모터/레이저 제어

STM32 펌웨어에서 사용되는 기본 값 예시:

```c
#define WIFI_SERVER_IP   "192.168.4.1"
#define WIFI_SERVER_PORT 5555
```

- 실제 환경에서는 **노트북 AP의 IP 주소**를 확인해서 `WIFI_SERVER_IP` 를 맞춰주거나,
  ESP에서 `CIPSTART` 할 때 그 IP를 직접 사용하면 됩니다.

---

## 2. Ubuntu 노트북을 Wi‑Fi AP(핫스팟)로 켜기

### 2.1 GUI(데스크탑 환경)에서 핫스팟 생성

1. 화면 우측 상단 **Wi‑Fi 아이콘 클릭 → Wi‑Fi 설정**으로 들어갑니다.
2. "..." 메뉴에서 **Hotspot / Wi‑Fi Hotspot 켜기** 를 선택합니다.
3. **SSID** 와 **비밀번호**를 적당히 정합니다. (예: SSID=`SFEPS_AP`, PW=`12345678`)
4. 핫스팟이 켜진 뒤, 터미널에서 AP 인터페이스 IP 확인:

   ```bash
   ip addr show
   ```

   - `wlp...` / `wlan0` 등의 인터페이스에서 `inet 192.168.x.y/24` 와 같은 주소를 찾습니다.
   - 이 IP(예: `192.168.4.1`)를 ESP/STM32 코드의 `WIFI_SERVER_IP` 로 사용하면 됩니다.

### 2.2 CLI로 nmcli를 사용해 핫스팟 생성

GUI가 없거나 터미널로만 작업하고 싶을 때:

```bash
# 실제 Wi‑Fi 인터페이스 이름 확인 (예: wlan0, wlp2s0 등)
ip link

# 예시: 인터페이스 이름이 wlan0인 경우
nmcli dev wifi hotspot ifname wlan0 ssid SFEPS_AP password "12345678"
```

- 위 명령을 실행하면 `SFEPS_AP` 라는 이름의 AP가 생성되고, 비밀번호는 `12345678` 입니다.
- `ip addr show wlan0` 로 IP를 확인하고, 그 주소를 ESP/STM32 통신에 사용합니다.

---

## 3. ESP8266 설정 (AT 명령 예시)

노트북 AP가 준비되었다고 가정하고, ESP8266을 AP에 붙이는 AT 명령 예시는 다음과 같습니다.

1. AP에 연결

   ```text
   AT
   AT+CWMODE=1                 // Station 모드
   AT+CWJAP="SFEPS_AP","12345678"  // 노트북 AP SSID / 비밀번호
   ```

   - 성공 시 `WIFI CONNECTED`, `WIFI GOT IP` 로그가 출력됩니다.

2. TCP 서버에 접속 (노트북 IP와 포트를 사용)

   ```text
   AT+CIPSTART="TCP","192.168.4.1",5555
   ```

   - `192.168.4.1` 부분은 실제 노트북 AP 인터페이스 IP로 교체.
   - STM32 코드의 `WIFI_SERVER_IP` 와 일치시키면 관리하기 편합니다.

3. 데이터 송수신 예시는 `../raspi_server/README.md` 의 `raspi_tcp_server` 설명과 동일하게 적용됩니다.

---

## 4. Ubuntu에서 TCP 서버 실행 (raspi_tcp_server 대체)

라즈베리 파이에서 사용하던 `raspi_tcp_server` 개념은 **그대로 노트북에서 실행**할 수 있습니다.

### 4.1 빌드 (예: 동일 소스를 Ubuntu에서 빌드할 때)

1. `tmp_server/raspi_server` 디렉터리로 이동:

   ```bash
   cd hardware/stm32-laser/tmp_server/raspi_server
   make raspi_tcp_server
   ```

2. 빌드가 성공하면 현재 디렉터리에 `raspi_tcp_server` 실행 파일이 생성됩니다.

### 4.2 Ubuntu에서 서버 실행

노트북 AP가 켜져 있고, ESP가 이 AP에 붙을 준비가 되어 있다고 가정합니다.

```bash
cd hardware/stm32-laser/tmp_server/raspi_server
./raspi_tcp_server          # 기본 포트 5555
# 또는 ./raspi_tcp_server 5555
```

- 서버는 `0.0.0.0:5555` 에서 클라이언트(ESP)를 기다립니다.
- ESP8266 쪽에서 `AT+CIPSTART="TCP","<노트북_IP>",5555` 를 실행하면 연결됩니다.

연결이 성공하면 터미널에는 라즈베리 파이와 동일하게:

```text
[TCP] Raspi TCP 서버 시작
[TCP] Listening on 0.0.0.0:5555
[TCP] Client connected from 192.168.4.x:포트
[TCP] 좌표 입력 예시: "0.5 0.3" (x y)
...
```

Ubuntu 노트북 상에서 **표준 입력으로 좌표나 문자열을 넣으면**, ESP/STM32 측으로 그대로 전달되어
레이저 제어나 디버그 출력에 사용할 수 있습니다.

---

## 5. 정리

- `raspi_server/` : 원래 라즈베리 파이에서 돌리던 RTSP/메타데이터/ESP 테스트 코드
- `ubuntu_server/` : 같은 개념을 **Ubuntu 노트북** 환경에서 사용할 때의 네트워크/서버 설정 메모
- 핵심 아이디어는 동일:
  - **ESP/STM32 쪽은 TCP 클라이언트**
  - **라즈베리/Ubuntu는 TCP 서버**
  - AP 역할은 환경에 따라 Pi 또는 노트북이 담당

