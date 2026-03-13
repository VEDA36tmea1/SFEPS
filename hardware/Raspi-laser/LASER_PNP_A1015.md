## A1015 PNP 트랜지스터로 레이저 제어하기 (GPIO17 기준)

이 문서는 라즈베리 파이에서 **A1015 PNP 트랜지스터**를 이용해 5V 레이저를 제어하는 방식을 정리한 것이다.  
`Laser_Detection_Delay.py` 및 `pid_pwm_agent.py` 와 같은 Raspi-laser 코드에서 참고한다.

---

### 1. A1015 핀 배열 (Pinout)

평평한 면(글자가 써진 쪽)을 **정면**에서 봤을 때, 왼쪽부터:

- **E (Emitter)**: 가장 왼쪽  
- **C (Collector)**: 가운데  
- **B (Base)**: 가장 오른쪽  

제조사/버전에 따라 C/B 위치가 바뀐 경우도 있으니, 실제 부품의 데이터시트(`KSA1015` 등)를 한 번 더 확인하는 것이 좋다.

---

### 2. A1015를 이용한 5V 레이저 제어 회로

PNP 트랜지스터는 **Emitter(E)를 높은 전압(여기서는 5V)** 에 묶어두고,  
Base 쪽 전압을 내려서 **전류를 “끌어오는” 방식**으로 사용한다.

권장 연결 방식:

- **Emitter (E)**: 라즈베리 파이의 **5V 핀** (물리 Pin 2 또는 4)
- **Collector (C)**: 레이저 모듈의 **(+) 단자**  
  - 레이저의 **(-)** 는 GND 로 연결
- **Base (B)**: 라즈베리 파이 **GPIO 핀** (예: BCM17)  
  - Base 와 GPIO 사이에 **1 kΩ 정도의 직렬 저항** 삽입 (베이스 전류 제한)
- 레이저(-)와 라즈베리 파이 GND는 **공통 접지**로 연결

회로 관점:

- E(5V) → C(레이저 +) → 레이저 내부 → GND 로 전류 흐름  
- Base 쪽을 얼마나 끌어내리느냐(LOW 수준) 에 따라 PNP 가 얼마나 세게 켜지는지가 결정된다.

---

### 3. 동작 특성 (GPIO 전압 vs 레이저 밝기)

라즈베리 파이 GPIO 는 **3.3V 논리**를 사용한다.  
Emitter 는 5V, GPIO High 는 3.3V 이므로:

- **GPIO = Low (0V)**  
  - Base–Emitter 간 전압 \(V_{EB} \approx 5V - 0V = 5V\)  
  - PNP 트랜지스터가 **강하게 ON** → 레이저가 **아주 밝게 켜짐**

- **GPIO = High (3.3V)**  
  - \(V_{EB} \approx 5V - 3.3V = 1.7V\)  
  - 여전히 어느 정도 전류가 흐를 수 있어, 레이저가 **완전히 꺼지지 않고 희미하게 켜질 가능성**이 큼

따라서 단순히 `GPIO.HIGH → OFF` 를 기대하면,  
실제로는 **“강한 ON ↔ 약한 ON”** 으로만 동작할 가능성이 크다.

---

### 4. 스크립트에서의 제어 전략 (모드 전환 방식)

위 문제를 해결하기 위해, `Laser_Detection_Delay.py` 에서는 **GPIO 모드 자체를 바꿔서 제어**한다.

핵심 아이디어:

- **강한 레이저 ON**:  
  - GPIO 를 **출력(OUT)** 으로 설정하고, **LOW(0)** 출력  
  - `GPIO.setup(PIN, GPIO.OUT); GPIO.output(PIN, GPIO.LOW)`

- **레이저 OFF (가능한 한 전류 차단)**:  
  - GPIO 를 **입력(IN, High-Z)** 로 설정  
  - `GPIO.setup(PIN, GPIO.IN)`

이렇게 하면:

- 레이저를 켤 때: 베이스를 강하게 GND 방향으로 끌어내려 **최대 밝기 ON**
- 레이저를 끌 때: 베이스를 **입력 모드(Hi-Z)** 로 두어, 베이스 전류를 거의 0으로 만들어 **사실상 OFF** 에 가깝게 만듦

코드 상의 예 (요약):

```python
PIN = 17  # BCM17

# OFF (입력 모드)
GPIO.setup(PIN, GPIO.IN)

# ON (강하게 ON)
GPIO.setup(PIN, GPIO.OUT)
GPIO.output(PIN, GPIO.LOW)

# 다시 OFF
GPIO.setup(PIN, GPIO.IN)
```

`Laser_Detection_Delay.py` 의 trial 루프에서는 이 패턴을 사용해서:

- 각 trial 시작 전: `GPIO.setup(pin, GPIO.IN)` → 레이저 OFF 안정 구간
- 측정 시작 시점: `GPIO.setup(pin, GPIO.OUT); GPIO.output(pin, GPIO.LOW)` → 레이저 ON
- 측정 후: 다시 `GPIO.setup(pin, GPIO.IN)` → 레이저 OFF

---

### 5. 요약

- A1015 PNP + 5V 레이저 회로에서 **GPIO LOW = 강한 ON**, **GPIO HIGH = 약한 ON** 이 될 수 있다.
- 완전히 끄고 켜는 동작을 위해서는, **GPIO 모드 전환(OUT/IN)** 을 활용하는 것이 효과적이다.
- Raspi-laser 코드에서는 **BCM17** 을 레이저 Enable 전용으로 사용하며, 모드 전환 패턴으로  
  **“강한 레이저 ON ↔ 사실상 OFF”** 를 구현해 실험용 지연 측정과 트래킹에 사용한다.

