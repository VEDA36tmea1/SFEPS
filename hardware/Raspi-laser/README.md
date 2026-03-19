# Raspi-laser (PWM 2ch + Laser GPIO)

라즈베리 파이를 이용해 **PWM 2채널**과 **레이저 ON/OFF(GPIO)** 를 안정적으로 뽑기 위한 핀 추천/배선/설정 메모입니다.

---

## 0) 주의 (필수)

- 라즈베리파이 GPIO는 **3.3V 전용**입니다. **5V를 GPIO에 직접 넣으면 즉시 고장**납니다.
- 레이저 모듈(특히 5V 구동)은 라즈베리파이 GPIO로 직접 구동하지 말고, **MOSFET/트랜지스터/드라이버**를 사용하세요.
- 외부 전원을 쓰든 라즈베리 5V를 쓰든, 제어 신호가 연결되면 **GND(접지)는 반드시 공통**으로 묶어야 합니다.

---

## 1) 추천 핀 구성 (기본안)

목표: **PWM 2개 + 5V OUT + Laser Enable(GPIO) 1개 + GND**

| 용도 | BCM(GPIO) | 물리 핀(40핀) | 비고 |
|---|---:|---:|---|
| PWM#0 | GPIO12 | Pin 32 | HW PWM0 (추천) |
| PWM#1 | GPIO13 | Pin 33 | HW PWM1 (추천) |
| Laser EN (ON/OFF) | GPIO23 | Pin 16 | 일반 GPIO (추천) |
| 5V OUT | 5V | Pin 2 또는 4 | 전류 여유 확인 |
| GND | GND | Pin 6 (또는 9/14/20/25/30/34/39) | 공통 접지 |

### 대체 핀(충돌 시)

- PWM0 대체: `GPIO18 (Pin 12)`
- PWM1 대체: `GPIO19 (Pin 35)`

단, 핀을 바꾸면 아래 `dtoverlay=pwm-2chan` 설정의 `pin/pin2`도 같이 바꿔야 합니다.

### PWM 초기값 (고정)

레이저/서보 제어용 **기본 듀티(펄스폭)** 는 아래로 고정한다.

| 축 | PWM 채널 | GPIO | 초기 펄스폭 | duty_cycle (ns) |
|---|:---:|:---:|---:|---:|
| x | pwm0 | GPIO12 | **1530 µs** | 1530000 |
| y | pwm1 | GPIO13 | **1300 µs** | 1300000 |

- 주기: 50 Hz (period = 20,000,000 ns)
- 설정 예:
  ```bash
  echo 20000000 | sudo tee /sys/class/pwm/pwmchip0/pwm0/period
  echo 20000000 | sudo tee /sys/class/pwm/pwmchip0/pwm1/period
  echo 1530000  | sudo tee /sys/class/pwm/pwmchip0/pwm0/duty_cycle
  echo 1300000  | sudo tee /sys/class/pwm/pwmchip0/pwm1/duty_cycle
  echo 1        | sudo tee /sys/class/pwm/pwmchip0/pwm0/enable
  echo 1        | sudo tee /sys/class/pwm/pwmchip0/pwm1/enable
  ```

---

## 2) 배선 가이드 (최소)

### PWM 출력(3.3V)

- 라즈베리파이 `GPIO12/13` → (필요 시) 드라이버 입력
- 상대 장치(드라이버/보드) GND ↔ 라즈베리 GND 공통

### 레이저 5V + Enable(추천)

**권장(안전)**: 레이저 전원(5V)은 라즈베리 5V 또는 외부 5V를 쓰고, Enable은 MOSFET로 스위칭

- 라즈베리 `GPIO23` → 게이트(저항 100~330Ω 권장)
- MOSFET 소스 → GND, 드레인 → 레이저(-), 레이저(+) → 5V
- 라즈베리 GND ↔ 레이저 전원 GND 공통

레이저 모듈이 “TTL/PWM 입력 핀”을 따로 제공하는 타입이면, 그 입력이 **3.3V tolerant인지 확인** 후 직접 연결하거나(가능한 경우), 레벨시프터를 사용하세요.

---

## 3) 라즈베리에서 PWM 2채널 활성화 (config.txt)

라즈베리 OS/버전에 따라 config 경로가 다릅니다.

- Bookworm(요즘): `/boot/firmware/config.txt`
- Legacy: `/boot/config.txt`

아래 내용을 해당 파일 맨 아래에 추가합니다(핀은 기본안 기준).

```ini
# Enable 2-channel hardware PWM on GPIO12 + GPIO13
dtoverlay=pwm-2chan,pin=12,func=4,pin2=13,func2=4
```

적용 후 재부팅:

```bash
sudo reboot
```

부팅 후 핀이 PWM ALT 모드인지 확인(옵션):

```bash
raspi-gpio get 12
raspi-gpio get 13
```

---

## 4) PWM 출력 방법 A: Linux PWM(sysfs)로 설정

부팅 후 PWM chip이 보이는지 확인:

```bash
ls /sys/class/pwm/
ls /sys/class/pwm/pwmchip0/
```

채널 export (보통 0,1 두 개가 잡힙니다):

```bash
echo 0 | sudo tee /sys/class/pwm/pwmchip0/export
echo 1 | sudo tee /sys/class/pwm/pwmchip0/export
```

### 예시 1) 20kHz, 듀티 50%

- period: 50µs = 50,000ns
- duty: 25,000ns

```bash
echo 50000  | sudo tee /sys/class/pwm/pwmchip0/pwm0/period
echo 25000  | sudo tee /sys/class/pwm/pwmchip0/pwm0/duty_cycle
echo 1      | sudo tee /sys/class/pwm/pwmchip0/pwm0/enable
```

### 예시 2) 50Hz(서보), 1.5ms 펄스(중앙)

- period: 20ms = 20,000,000ns
- duty: 1.5ms = 1,500,000ns

```bash
echo 20000000 | sudo tee /sys/class/pwm/pwmchip0/pwm0/period
echo 1500000  | sudo tee /sys/class/pwm/pwmchip0/pwm0/duty_cycle
echo 1        | sudo tee /sys/class/pwm/pwmchip0/pwm0/enable
```

정지:

```bash
echo 0 | sudo tee /sys/class/pwm/pwmchip0/pwm0/enable
echo 0 | sudo tee /sys/class/pwm/pwmchip0/pwm1/enable
```

---

## 5) 레이저 ON/OFF 방법: libgpiod(gpioset)

패키지 설치:

```bash
sudo apt update
sudo apt install -y gpiod
```

GPIO23을 HIGH(ON), LOW(OFF)로 토글:

```bash
sudo gpioset gpiochip0 23=1   # ON
sudo gpioset gpiochip0 23=0   # OFF
```

고정 출력(백그라운드 유지) 방식이 필요하면, 사용 중인 배포판/커널의 `gpioset` 옵션(`--mode=signal` 등)을 확인해서 “유지 모드”로 쓰는 걸 권장합니다.

---

## 6) Ubuntu 파이프라인 + 50Hz PID/PWM 제어

호스트(host_cpp)와 Ubuntu TCP 서버가 보내는 **레이저–바운딩박스 오차(EX, EY)** 를 라즈베리에서 수신해, **20ms(50Hz) 주기로 PID 연산 후 pwm0/pwm1** 에 반영하는 에이전트가 포함되어 있습니다.

- **[PIPELINE.md](PIPELINE.md)** — 데이터 흐름, Ubuntu/라즈베리 실행 순서, 게인 튜닝 요약
- **pid_pwm_agent.py** — TCP 클라이언트(노트북 IP:5555 접속) + 50Hz PID 루프 + sysfs PWM 출력

실행 예 (노트북 AP IP가 10.42.0.1 일 때):

```bash
sudo python3 pid_pwm_agent.py --host 10.42.0.1 --port 5555
```

초기 PWM은 위 “PWM 초기값 (고정)” (x=1530µs, y=1300µs)을 사용하며, `--kp-x`, `--kp-y` 등으로 게인을 바로 바꿔가며 실시간 튜닝할 수 있습니다.

---

## 7) 수동 테스트: 키보드 화살표로 PWM 조절

`GPIO12(pwm0)` / `GPIO13(pwm1)` 을 키보드 화살표로 직접 올리고 내리며 테스트합니다.

```bash
sudo systemctl enable --now pigpiod
python3 keyboard_pwm_control.py
```

- `←/→`: PWM0(GPIO12) 감소/증가
- `↑/↓`: PWM1(GPIO13) 증가/감소
- `m`: 커플링 상쇄 모드 토글(NORMAL ↔ DECOUPLE)
- `q`: 종료 (pigpio: pulsewidth=0, sysfs: enable=0)

커플링 상쇄 모드로 시작(사용자 실측값 기반 기본 \(J\) 포함):

```bash
python3 keyboard_pwm_control.py --decouple
```

sysfs 백엔드로 강제하려면(기존 방식):

```bash
sudo python3 keyboard_pwm_control.py --backend sysfs
```

---

## 8) 축 커플링 보정(디커플링) 수식

- **[DECOUPLING_CALIBRATION.md](DECOUPLING_CALIBRATION.md)** — \(J\) 추정(유한차분)과 \(\Delta u=-\alpha J^{-1}e\) 제어식 정리

---

## 9) camera_RBF의 `SET_PWM` 수신해서 바로 PWM 출력(라즈베리)

`Camera/get_metadata/src/camera_RBF.cpp`는 stdout으로 `SET_PWM,PAN=...,TILT=...`를 출력하고, 우분투의 `ubuntu_tcp_server`가 그 라인을 라즈베리로 전달합니다.  
라즈베리에서는 아래 클라이언트를 실행하면 **SET_PWM을 받는 즉시 GPIO12/13 PWM을 갱신**합니다.

pigpio(권장, DMA 기반):

```bash
sudo systemctl enable --now pigpiod
python3 set_pwm_client.py --host 10.42.0.1 --port 5555 -v
```

sysfs(기존 방식):

```bash
sudo python3 set_pwm_client.py --backend sysfs --host 10.42.0.1 --port 5555 -v
```

