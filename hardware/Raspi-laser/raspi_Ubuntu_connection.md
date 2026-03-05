# Raspi ↔ Ubuntu 노트북 Wi‑Fi 연결 (WPA2, nmcli)

라즈베리 파이(우분투 계열)를 **Ubuntu 노트북 핫스팟(SFEPS_AP2)** 에 WPA2 로 붙인 과정을 정리한 메모입니다.

---

## 1. 환경

- **노트북**: Ubuntu, `NetworkManager` 사용
  - 무선 인터페이스: `wlo1`
  - 핫스팟 SSID: `SFEPS_AP2`
  - 비밀번호: `chl571010`
  - 대역: 2.4GHz (band `bg`)
  - 보안: **WPA2-Personal (RSN, CCMP)** 로 고정
- **라즈베리 파이**: Ubuntu (또는 데비안 계열)  
  - 무선 인터페이스: `wlan0`
  - `NetworkManager` + `nmcli` 사용

---

## 2. 노트북에서 WPA2 전용 핫스팟 설정

노트북(ROS2용 Ubuntu)에서 터미널로 `NetworkManager` 핫스팟 프로파일을 직접 수정.

```bash
sudo nmcli connection modify Hotspot \
  802-11-wireless.band bg \
  802-11-wireless-security.key-mgmt wpa-psk \
  802-11-wireless-security.proto rsn \
  802-11-wireless-security.pairwise ccmp \
  802-11-wireless-security.group ccmp

# 핫스팟 재시작
sudo nmcli connection down Hotspot
sudo nmcli connection up Hotspot
```

핫스팟이 켜진 뒤, SSID/보안 확인:

```bash
nmcli dev wifi list | grep SFEPS_AP2
```

- `SECURITY` 컬럼에 **WPA2 (또는 WPA2 WPA3)** 가 보이지만,  
  위 설정으로 **실제 운용은 WPA2-PSK(RSN/CCMP)** 기준이 되게 맞춰둠.

---

## 3. 라즈베리 파이에서 NetworkManager 활성화 (한 번만)

라즈베리에서 `NetworkManager` 를 설치/활성화(이미 설치되어 있을 수 있음).

```bash
sudo apt update
sudo apt install -y network-manager

# 서비스 실행 + 부팅 시 자동 시작
sudo systemctl enable --now NetworkManager
```

추가로, 예전에 AP 모드/직접 wpa_supplicant 를 썼다면 충돌 방지를 위해:

```bash
sudo systemctl stop wpa_supplicant wpa_supplicant@wlan0 hostapd dnsmasq 2>/dev/null
sudo rm -f /run/wpa_supplicant/wlan0 /var/run/wpa_supplicant/wlan0
```

---

## 4. 라즈베리에서 노트북 핫스팟(SFEPS_AP2)에 연결

`nmcli` 로 직접 SSID/비번을 지정해서 접속:

```bash
sudo nmcli dev wifi connect "SFEPS_AP2" password "chl571010" ifname wlan0
```

정상 연결 시:

```text
Device 'wlan0' successfully activated with '<UUID>'.
```

상태 확인:

```bash
iwgetid
ip addr show wlan0
```

예상 형태:

```text
wlan0     ESSID:"SFEPS_AP2"
3: wlan0: ... state UP ...
    inet 10.42.0.x/24 brd 10.42.0.255 scope global wlan0
```

- **라즈베리 IP**: `10.42.0.x` (예: `10.42.0.123`)
- **노트북 IP**: 보통 `10.42.0.1` (노트북 쪽 `wlo1` 확인)

---

## 5. 통신 테스트 (핑)

### 라즈베리 → 노트북

```bash
ping -c 4 10.42.0.1
```

### 노트북 → 라즈베리

```bash
ping -c 4 10.42.0.x    # 위에서 확인한 라즈베리 IP
```

두 방향 모두 ping 이 통과하면, **TCP/UDP 통신(예: raspi_server, PID 튜닝 데이터 전송)** 을 바로 올릴 수 있는 상태다.

---

## 6. 트러블슈팅 메모

- **NetworkManager is not running**
  - `nmcli` 사용 전:
    ```bash
    sudo systemctl enable --now NetworkManager
    ```
- **이전 AP/직접 wpa_supplicant 설정과 충돌**
  - `wpa_supplicant@wlan0`, `hostapd`, `dnsmasq` 가 동시에 wlan0을 잡고 있으면  
    `Match already configured`, `ctrl_iface exists` 류 에러가 발생 → 위 3단계처럼 정리 후 NetworkManager만 사용.

