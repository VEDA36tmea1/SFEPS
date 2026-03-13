## LUT check 모드 (host → Raspi 직접 PWM/레이저 제어)

`pid_pwm_agent.py`의 **`--lut-check` 모드**는 호스트(노트북/우분투)가 라즈베리에게
직접 PWM 값을 지정(`SET_PWM`)하고 레이저 ON/OFF(`LASER`)를 시키는 디버그/점검 모드다.

- PID / LUT-track / LUT 수집 로직이 **PWM을 덮어쓰지 않도록** 차단한다.
- 즉, `SET_PWM`으로 준 값이 그대로 sysfs PWM에 유지된다.

---

### 실행 (라즈베리)

PWM(sysfs)과 레이저 GPIO를 실제로 제어하려면 `sudo` 권한이 필요하다.

```bash
cd ~/SFEPS/hardware/Raspi-laser

sudo python3 pid_pwm_agent.py --host 192.168.0.44 --port 5555 \
  --lut-check \
  --laser-pin 17 --laser-on-start
```

- `--host`: 우분투 `ubuntu_tcp_server`가 떠 있는 IP
- `--port`: 서버 포트 (기본 5555)
- `--lut-check`: LUT 체크 모드 활성화 (자동 제어 OFF)
- `--laser-pin 17`: 레이저 Enable GPIO (BCM 번호). A1015 PNP 구성 기준으로 사용
- `--laser-on-start`: 시작 시 레이저 ON (원치 않으면 옵션 제거)

---

### 호스트에서 보내는 명령 포맷

라즈베리가 수신할 수 있는 텍스트 라인 명령은 다음과 같다(줄 끝에 `\\n` 포함).

#### 1) PWM 직접 지정

```text
SET_PWM,PAN=<us>,TILT=<us>
```

예:

```text
SET_PWM,PAN=1062,TILT=1607
```

- 단위: **µs**
- Raspi는 수신 즉시 sysfs PWM에 duty_cycle을 갱신한다.

#### 2) 레이저 ON/OFF

```text
LASER,ON
LASER,OFF
```

또는 숫자:

```text
LASER,1
LASER,0
```

A1015 PNP 구성 기준:

- **ON**: GPIO를 OUT으로 설정 후 LOW 출력 (강하게 켜짐)
- **OFF**: GPIO를 IN(Hi-Z)로 전환 (확실히 꺼짐에 가깝게)

#### 3) 현재 PWM 값 요청/응답

요청:

```text
REQUEST_PWM,GR=<r>,GC=<c>
```

응답(라즈베리 → 호스트):

```text
PAN=<us>,TILT=<us>
```

---

### 동작 확인 (라즈베리)

라즈베리에서 sysfs PWM 값이 실제로 바뀌었는지 확인:

```bash
cat /sys/class/pwm/pwmchip0/pwm0/duty_cycle
cat /sys/class/pwm/pwmchip0/pwm1/duty_cycle
```

예: `SET_PWM,PAN=1062,TILT=1607` 을 보냈다면

- `pwm0/duty_cycle = 1062000` (ns)
- `pwm1/duty_cycle = 1607000` (ns)

---

### 주의

- `--lut-check`는 **점검 모드**이므로, 실시간 트래킹/제어용으로는 `--lut-track` 또는 PID 모드를 사용한다.
- 라즈베리 PWM sysfs는 `sudo` 권한이 필요하다.

