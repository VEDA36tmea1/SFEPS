# Raspi-laser 핀 연결 (40-pin 헤더 기준)

기본 추천 배선(2채널 PWM + Laser EN + 5V + GND) 요약입니다.

---

## 1) 연결 요약 (추천)

| 신호 | 라즈베리 물리 핀 | BCM(GPIO) | 전기적 레벨 |
|---|---:|---:|---|
| 5V OUT | 2 또는 4 | - | 5V |
| GND | 6 (또는 아무 GND) | - | 0V |
| PWM#0 | 32 | GPIO12 | 3.3V |
| PWM#1 | 33 | GPIO13 | 3.3V |
| Laser EN | 16 | GPIO23 | 3.3V |

---

## 2) 40핀 헤더에서 위치 감(일부만 표시)

왼쪽이 홀수, 오른쪽이 짝수(보드 기준)입니다.

```text
(보드 상단)
  (1) 3V3     (2) 5V      <- 5V OUT (pin 2)
  (3) SDA1    (4) 5V      <- 5V OUT (pin 4)
  (5) SCL1    (6) GND     <- GND (pin 6)
  ...
 (12) GPIO18  (13) GPIO27
  ...
 (16) GPIO23  (17) 3V3    <- Laser EN = GPIO23 (pin 16)
  ...
 (32) GPIO12  (33) GPIO13 <- PWM#0 = GPIO12 (pin 32), PWM#1 = GPIO13 (pin 33)
 (34) GND     (35) GPIO19
  ...
(보드 하단)
```

---

## 3) 대체 핀(필요 시)

- PWM0 대체: `GPIO12 (pin 32)`
- PWM1 대체: `GPIO19 (pin 35)`

대체 핀을 쓰면 `config.txt`의 `dtoverlay=pwm-2chan`의 `pin/pin2` 값을 같이 변경해야 합니다.

