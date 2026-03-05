# PID_control (오프라인 자동 튜닝 가이드)

이 문서는 현재 프로젝트(30FPS 영상 + 레이저 pointing + 오버슈트 존재) 기준으로,
STM32 PID 게인을 오프라인에서 자동 탐색하는 방법을 정리한다.

핵심 전략:
- 실시간 제어는 STM32에서 수행
- 로그(`PIDLOG,...`)를 수집해 PC에서 시스템 식별/자동 튜닝
- 결과 게인(Kp/Ki/Kd)을 다시 STM32에 반영

---

## 1) 폴더 구성

- `requirements.txt`: Python 라이브러리 목록
- `auto_tune_pid.py`: 로그 기반 자동 PID 게인 탐색 스크립트

---

## 2) 라이브러리 설치

프로젝트 루트 기준:

```bash
cd "/home/ros2man/Desktop/SFEPS/hardware/stm32-laser/PID_control"
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

단일 명령으로 설치만 빠르게 하려면:

```bash
pip install numpy pandas matplotlib scipy control pyserial opencv-python
```

---

## 3) STM32 로그 기록 코드 (이미 main.c 반영됨)

현재 `stm32/Core/Src/main.c` 에 아래 형태로 로그가 출력되도록 추가되어 있다.

- 로그 예시:
  - `PIDLOG,t:12345,ex:-10.00,ey:5.00,out_x:1432,out_y:1251,kpx:0.0100,kix:0.0000,kdx:0.0000,kpy:-0.0100,kiy:0.0000,kdy:0.0000`

- 관련 매크로:
  - `IBVS_PID_LOG_ENABLE` (1: 로그 출력, 0: 비활성)
  - `IBVS_PID_LOG_PERIOD_MS` (기본 100ms)

필요하면 주기만 변경:

```c
#define IBVS_PID_LOG_PERIOD_MS  50u   // 더 촘촘한 로그
```

---

## 4) 로그 수집 방법

1. STM32 플래시 후, 시리얼 모니터(USART2) 실행
2. 레이저 트래킹을 1~2분 동작
3. 터미널 출력을 파일로 저장 (예: `pid_log.txt`)

예시 (환경에 맞게 포트 수정):

```bash
python3 -m serial.tools.miniterm /dev/ttyACM0 115200 | tee pid_log.txt
```

---

## 5) 자동 튜닝 실행

```bash
cd "/home/ros2man/Desktop/SFEPS/hardware/stm32-laser/PID_control"
source .venv/bin/activate
python3 auto_tune_pid.py --log pid_log.txt --plot
```

출력:
- X축/PA0 권장 `kp,ki,kd`
- Y축/PA8 권장 `kp,ki,kd` (현재 부호 규약 반영해서 음수로 출력)
- `IbvsPid_AxisInit(...)` 에 바로 붙여넣을 수 있는 코드 라인

---

## 6) STM32 반영 위치

`stm32/Core/Src/main.c` -> `IbvsPid_Init()`:

```c
IbvsPid_AxisInit(&pid_x, /* Kp */, /* Ki */, /* Kd */, (float)ux_init);
IbvsPid_AxisInit(&pid_y, /* Kp */, /* Ki */, /* Kd */, (float)uy_init);
```

주의:
- 현재 프로젝트 축 규약은
  - X(PA0): 오른쪽으로 갈수록 PWM 증가 -> `Kp_x > 0`
  - Y(PA8): 아래로 갈수록 PWM 감소 -> `Kp_y < 0`
- 스크립트 출력은 이 규약을 반영한 부호로 제공된다.

---

## 7) 추천 튜닝 순서

1. 먼저 `Ki=0`, `Kd=0` 에서 `Kp` 안정화
2. `Ki` 소량 추가 (정특성 오차 제거)
3. `Kd` 소량 추가 (오버슈트 감쇠)
4. 과진동 시:
   - `Kp` 소폭 하향
   - `IBVS_UPDATE_PERIOD_MS` 증가(예: 15 -> 20/25)
   - `SEND_EVERY_N_FRAMES` 조정(호스트 전송 주기)

