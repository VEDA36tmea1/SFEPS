# 라즈베리 파이 AP 모드 설정 (ESP8266 ↔ STM32 ↔ Pi 통신)

STM32 Nucleo-F401RE에 ESP8266을 붙이고, 라즈베리 파이를 **AP(공유기)** 로 켜서 ESP8266이 Pi의 Wi‑Fi에 접속·통신할 수 있게 하는 구성입니다.

---

## 1. 연결 개요

### 1.1 ESP8266 ↔ Nucleo-F401RE

| ESP8266 핀 | Nucleo-F401RE 핀 | 비고 |
|------------|------------------|------|
| VBUS (5V)  | 5V               | 전원 (또는 3.3V 사용 시 3V3) |
| GND        | GND              | 공통 GND |
| **TX**     | **D2 (PA10)**    | Nucleo **UART RX** (ESP TX → PA10) |
| **RX**     | **D8 (PA9)**     | Nucleo **UART TX** (PA9 → ESP RX) |

- Nucleo 기준: **PA9 = USART1_TX**, **PA10 = USART1_RX** (STM32 UART1 사용).
- ESP8266은 3.3V 논리이므로, Nucleo(3.3V)와 직접 연결 가능. 5V 전원만 쓰고 신호선은 3.3V 유지.

### 1.2 네트워크 구조

```
[인터넷] ←랜선(eth0)→ [라즈베리 파이] ←Wi‑Fi AP(wlan0)→ [ESP8266] ←UART→ [STM32 Nucleo]
```

- **라즈베리 파이**: 랜선(eth0)으로 외부 인터넷, **wlan0를 AP**로 켜서 ESP8266/STM32가 접속.
- **ESP8266**: Pi가 만든 AP에 Wi‑Fi로 연결 후, Pi 위에서 도는 TCP 서버 등과 통신.

---

## 2. 라즈베리 파이 AP 모드 구성

아래 두 가지 중 하나만 하면 됩니다.

---

### 방법 A: RaspAP으로 빠르게 구성 (권장)

1. **RaspAP 설치**

   ```bash
   sudo apt update
   sudo apt install -y raspap-webgui
   ```

   설치 중 “Configure RaspAP”에서 **Yes** 선택 후, 필요하면 재부팅.

2. **웹 설정**
   - Pi의 **eth0 IP**로 브라우저 접속 (예: `http://192.168.0.xxx`).
   - 로그인: 계정 `admin`, 비밀번호 `changeme` (최초 로그인 후 반드시 변경).
   - **WiFi** → **Enable AP** 켜기.
   - **SSID**, **비밀번호(WPA)** 설정 후 저장.

3. **동작 확인**
   - 스마트폰/PC로 해당 SSID 검색 후 연결해 보기.
   - ESP8266도 이 SSID/비밀번호로 연결하면 됨.

---

### 방법 B: hostapd + dnsmasq 로 수동 구성

#### 2.1 패키지 설치

```bash
sudo apt update
sudo apt install -y hostapd dnsmasq
sudo systemctl stop hostapd
sudo systemctl stop dnsmasq
```

#### 2.2 고정 IP (wlan0, AP용)

`/etc/dhcpcd.conf` 끝에 추가:

```bash
# AP용 wlan0 고정 IP (eth0는 기존대로 랜선 DHCP 유지)
interface wlan0
    static ip_address=192.168.4.1/24
    nohook wpa_supplicant
```

적용:

```bash
sudo systemctl restart dhcpcd
```

#### 2.3 dnsmasq (AP 클라이언트에게 IP 배포)

다른 설정과 겹치지 않도록 기존 설정 백업 후, AP 전용 설정만 사용:

```bash
sudo mv /etc/dnsmasq.conf /etc/dnsmasq.conf.bak
```

`/etc/dnsmasq.conf` 새로 만들기:

```ini
interface=wlan0
dhcp-range=192.168.4.2,192.168.4.20,255.255.255.0,24h
domain=local
address=/raspi.local/192.168.4.1
```

#### 2.4 hostapd (AP 설정)

`/etc/hostapd/hostapd.conf`:

```ini
interface=wlan0
driver=nl80211
ssid=SFEPS_AP
hw_mode=g
channel=6
wmm_enabled=0
macaddr_acl=0
auth_algs=1
ignore_broadcast_ssid=0
wpa=2
wpa_passphrase=your_password_here
wpa_key_mgmt=WPA-PSK
wpa_pairwise=TKIP
rsn_pairwise=CCMP
```

- `ssid`: ESP8266/STM32가 접속할 AP 이름.
- `wpa_passphrase`: AP 비밀번호 (원하는 값으로 변경).
<span style="color: orange">-> 최소 8자리 이상으로 설정해야한다 .!! </span>


hostapd가 이 파일을 쓰도록:

```bash
sudo bash -c 'echo DAEMON_CONF=\"/etc/hostapd/hostapd.conf\" >> /etc/default/hostapd'
sudo systemctl unmask hostapd
sudo systemctl enable hostapd
sudo systemctl start hostapd
sudo systemctl start dnsmasq
```

#### 2.5 IP 포워딩 (선택, AP 클라이언트가 인터넷 쓰게 할 때)

AP 클라이언트(ESP8266 등)가 Pi를 통해 인터넷에 나가게 하려면:

```bash
sudo sed -i 's/#net.ipv4.ip_forward=1/net.ipv4.ip_forward=1/' /etc/sysctl.conf
sudo sh -c "echo 1 > /proc/sys/net/ipv4/ip_forward"
sudo iptables -t nat -A POSTROUTING -o eth0 -j MASQUERADE
sudo iptables -A FORWARD -i eth0 -o wlan0 -m state --state RELATED,ESTABLISHED -j ACCEPT
sudo iptables -A FORWARD -i wlan0 -o eth0 -j ACCEPT
```

재부팅 후에도 유지하려면 `iptables-persistent` 등으로 규칙 저장하는 것이 좋습니다.

---

## 3. 통신 흐름 (ESP8266 ↔ Pi)

1. **ESP8266**: Pi의 AP(예: `SFEPS_AP`)에 연결 → DHCP로 `192.168.4.x` 대역 IP 획득.
2. **Pi**: AP의 게이트웨이 = `192.168.4.1`. 이 IP에서 **TCP 서버**(또는 UDP)를 열어 두면 됨.
3. **STM32**: UART로 ESP8266에 AT 명령 또는 펌웨어에서 정의한 프로토콜로 “Pi의 192.168.4.1:포트”로 접속하라고 지시.
4. **Pi 서버**: `tmp_raspi_server` 또는 기존 서버 코드에서 해당 포트 listen → STM32와 메시지 주고받기.

예시 (Pi에서 테스트용 TCP 서버):

```bash
# 192.168.4.1:5555 에서 대기 (실제 서비스는 Python/C 등으로 구현)
nc -lk 5555
```

ESP8266/STM32는 `192.168.4.1` 포트 `5555`로 접속하면 됨.

---

## 4. 정리

| 항목 | 내용 |
|------|------|
| **연결** | ESP8266 TX→PA10(RX), RX→PA9(TX), GND, 5V(또는 3V3) |
| **Pi 역할** | 랜선(eth0)=인터넷, wlan0=AP로 ESP8266/STM32 접속 허용 |
| **AP 구성** | RaspAP(방법 A) 또는 hostapd+dnsmasq(방법 B) |
| **통신** | ESP8266이 Pi AP에 접속 → Pi의 192.168.4.1:원하는포트 로 TCP/UDP |

이후 STM32 쪽에서는 USART1(PA9/PA10)로 ESP8266 AT 명령 또는 커스텀 프로토콜을 구현하면 됩니다.
