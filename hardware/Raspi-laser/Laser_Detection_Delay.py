#!/usr/bin/env python3
"""
Laser_Detection_Delay.py

라즈베리 파이에서 레이저 Enable GPIO를 토글하면서,
Ubuntu 호스트 파이프라인(ubuntu_tcp_server → pid_pwm_agent.py 와 동일 형식)이
레이저를 감지해 EX/EY 라인을 보내기까지 걸리는 지연 시간을 측정한다.

- PWM/서보는 pid_pwm_agent.py 와 동일하게 sysfs PWM 을 초기화해 중심 위치(1530µs,1300µs)로 고정.
- Laser EN 은 기본으로 **BCM GPIO17**(사용자가 +를 물려둔 핀, A1015 PNP로 구성되어 LOW=ON) 을 사용하지만,
  --pin 으로 변경 가능.
- 호스트는 ubuntu_tcp_server 가 떠 있고, rtsp_laser_demo/vision 파이프라인이
  레이저를 감지하면 "EX=...,EY=..." 형식의 라인을 TCP 로 보내고 있다고 가정한다.

실행 예:

  # 호스트: rtsp_laser_demo(레이저 탐지) → 파이프 → ubuntu_tcp_server
  ./rtsp_laser_demo --nolut --gst | ../../tmp_server/ubuntu_server/ubuntu_tcp_server

  # 라즈베리: TCP로 서버에 연결 후 Enter 로 레이저 ON/OFF 하며 지연 측정
  sudo python3 Laser_Detection_Delay.py --host 192.168.0.44 --port 5555 \\
      --pin 23 --trials 10

  --nolut 모드에서는 박스 없이 레이저만 감지돼도 EX/EY 라인이 전송되므로
  지연 측정 시 호스트에서 별도 타겟 박스를 그리지 않아도 된다.

"""

from __future__ import annotations

import argparse
import re
import os
import socket
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import List

try:
    import RPi.GPIO as GPIO
except ImportError as e:  # pragma: no cover - Raspi 전용
    GPIO = None  # type: ignore


EXEY_RE = re.compile(r"EX=([-\d.]+),EY=([-\d.]+)")

# pid_pwm_agent.py 와 동일한 PWM 설정
PWM_PERIOD_NS = 20_000_000
INIT_X_US = 1530
INIT_Y_US = 1300
SYSFS_PWM_CHIP = "/sys/class/pwm/pwmchip0"
SYSFS_PWM0 = f"{SYSFS_PWM_CHIP}/pwm0"
SYSFS_PWM1 = f"{SYSFS_PWM_CHIP}/pwm1"


def us_to_ns(us: float) -> int:
    return int(round(us * 1000.0))


def ensure_pwm_exported_and_center() -> None:
    """pwm0/pwm1 을 export 하고 period=20ms, enable=1, center 듀티로 설정."""
    # export 시도
    for ch in (0, 1):
        export_path = f"{SYSFS_PWM_CHIP}/export"
        try:
            with open(export_path, "w") as f:
                f.write(str(ch))
        except (OSError, FileNotFoundError):
            pass
    time.sleep(0.15)

    # period 설정
    for ch in (0, 1):
        p = SYSFS_PWM0 if ch == 0 else SYSFS_PWM1
        period_path = f"{p}/period"
        if not os.path.exists(period_path):
            sys.stderr.write(f"[delay] Warning: {period_path} not found. sudo 권한/overlay 확인.\n")
            continue
        try:
            with open(period_path, "w") as f:
                f.write(str(PWM_PERIOD_NS))
        except OSError as e:
            sys.stderr.write(f"[delay] period 설정 실패({p}): {e}\n")
    time.sleep(0.05)

    # center 듀티 + enable
    centers = {0: INIT_X_US, 1: INIT_Y_US}
    for ch in (0, 1):
        p = SYSFS_PWM0 if ch == 0 else SYSFS_PWM1
        duty_path = f"{p}/duty_cycle"
        enable_path = f"{p}/enable"
        if os.path.exists(duty_path):
            try:
                with open(duty_path, "w") as f:
                    f.write(str(us_to_ns(centers[ch])))
            except OSError as e:
                sys.stderr.write(f"[delay] duty 설정 실패({p}): {e}\n")
        if os.path.exists(enable_path):
            try:
                with open(enable_path, "w") as f:
                    f.write("1")
            except OSError as e:
                sys.stderr.write(f"[delay] enable 실패({p}): {e}\n")


@dataclass
class SharedState:
    """수신 스레드와 메인 루프가 공유하는 감지 시각/카운터."""

    last_t: float = 0.0
    count: int = 0
    lock: threading.Lock = field(default_factory=threading.Lock)


def recv_loop(sock: socket.socket, shared: SharedState) -> None:
    """ubuntu_tcp_server 에서 오는 EX/EY 라인을 감지해 시각 기록."""
    buf = b""
    while True:
        try:
            data = sock.recv(256)
            if not data:
                break
            buf += data
            while b"\n" in buf:
                line, _, buf = buf.partition(b"\n")
                line = line.strip().decode("utf-8", errors="ignore").strip()
                if not line:
                    continue
                m = EXEY_RE.search(line)
                if m:
                    now = time.monotonic()
                    try:
                        ex = float(m.group(1))
                        ey = float(m.group(2))
                    except ValueError:
                        ex = ey = 0.0
                    with shared.lock:
                        shared.last_t = now
                        shared.count += 1
                    # 디버그용으로 stderr 에 짧게 출력
                    sys.stderr.write(
                        f"[delay] RX EX={ex:.1f} EY={ey:.1f} (count={shared.count})\n"
                    )
        except socket.timeout:
            continue
        except (ConnectionResetError, BrokenPipeError, OSError):
            break
    sys.stderr.write("[delay] recv_loop ended.\n")


def measure_delays(
    host: str,
    port: int,
    pin: int,
    timeout_s: float,
) -> None:
    if GPIO is None:
        sys.stderr.write("[delay] RPi.GPIO 모듈을 불러올 수 없습니다. 라즈베리 파이에서 sudo 로 실행하세요.\n")
        sys.exit(1)

    # PWM을 pid_pwm_agent 와 동일하게 초기화 (서보/헤드 위치 고정)
    ensure_pwm_exported_and_center()

    # GPIO 설정 (BCM 번호 기준, Laser EN)
    # A1015 PNP + 5V 구성:
    #   - GPIO LOW + 출력(OUT) → 베이스를 강하게 끌어내려 레이저 "강하게 ON"
    #   - GPIO 입력(IN, Hi-Z)  → 베이스에 전류가 거의 안 흘러 레이저 "OFF" 에 가깝게
    #
    # 따라서 스크립트 안에서는
    #   ON  : GPIO.setup(pin, OUT); GPIO.output(pin, LOW)
    #   OFF : GPIO.setup(pin, IN)   (모드 변경)
    # 로 강한 ON ↔ 확실한 OFF 를 구현한다.
    GPIO.setmode(GPIO.BCM)
    GPIO.setup(pin, GPIO.IN)  # 시작 시 OFF

    # TCP 연결
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10.0)
    try:
        sock.connect((host, port))
    except (socket.error, OSError) as e:
        sys.stderr.write(f"[delay] TCP connect 실패: {e}\n")
        GPIO.cleanup()
        sys.exit(1)
    sock.settimeout(0.5)

    shared = SharedState()
    th = threading.Thread(target=recv_loop, args=(sock, shared), daemon=True)
    th.start()

    delays: List[float] = []
    laser_on = False  # 엔터로 토글: OFF ↔ ON
    trial_idx = 0
    try:
        sys.stderr.write(
            "\n[delay] Enter = 레이저 ON/OFF 토글. "
            "켜진 순간에 호스트 EX/EY 수신까지 지연을 측정합니다. (q = 종료)\n"
        )
        while True:
            cmd = input("\n[delay] Enter=토글, q=종료 > ").strip().lower()
            if cmd.startswith("q"):
                if laser_on:
                    GPIO.setup(pin, GPIO.IN)
                break

            if laser_on:
                # 지금 ON → 엔터 시 OFF
                GPIO.setup(pin, GPIO.IN)
                laser_on = False
                sys.stderr.write(f"[delay] 레이저 OFF (GPIO{pin})\n")
            else:
                # 지금 OFF → 엔터 시 ON + 지연 측정
                trial_idx += 1
                sys.stderr.write(f"\n[delay] ==== Trial {trial_idx} (레이저 ON) ====\n")

                with shared.lock:
                    base_count = shared.count

                t_on = time.monotonic()
                GPIO.setup(pin, GPIO.OUT)
                GPIO.output(pin, GPIO.LOW)
                laser_on = True
                sys.stderr.write(f"[delay] 레이저 ON (GPIO{pin}) at t={t_on:.6f}\n")

                det_t = None
                deadline = t_on + timeout_s
                while time.monotonic() < deadline:
                    time.sleep(0.001)
                    with shared.lock:
                        c = shared.count
                        lt = shared.last_t
                    if c > base_count and lt >= t_on:
                        det_t = lt
                        break

                if det_t is None:
                    sys.stderr.write("[delay] 타임아웃: 감지 신호를 받지 못했습니다.\n")
                else:
                    dt = det_t - t_on
                    delays.append(dt)
                    delay_ms = dt * 1000.0
                    sys.stderr.write(f"[delay] 켠 시각 ~ 수신 시각 차이: {delay_ms:.1f} ms\n")
                    print(f"TRIAL,{trial_idx},delay_ms,{delay_ms:.3f}")
                # 레이저는 ON 상태 유지 (다음 엔터에 OFF)

        # 요약 통계 (원하면 나중에 CSV 로도 분석 가능)
        if delays:
            avg = sum(delays) / len(delays)
            d_min = min(delays)
            d_max = max(delays)
            print("\nRESULT, trials,", len(delays))
            print(f"RESULT, avg_ms,{avg*1000.0:.3f}")
            print(f"RESULT, min_ms,{d_min*1000.0:.3f}")
            print(f"RESULT, max_ms,{d_max*1000.0:.3f}")
        else:
            print("RESULT, no_successful_trials,0")
    finally:
        try:
            sock.close()
        except OSError:
            pass
        GPIO.cleanup()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="레이저 ON 후 Ubuntu 파이프라인이 EX/EY 를 보내기까지의 지연 시간 측정기"
    )
    parser.add_argument("--host", default="10.42.0.1", help="Ubuntu TCP 서버 IP")
    parser.add_argument("--port", type=int, default=5555, help="Ubuntu TCP 서버 포트")
    parser.add_argument(
        "--pin",
        type=int,
        default=17,
        help="Laser Enable 에 사용할 BCM GPIO 번호 (기본: 17)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=2.0,
        help="각 trial 당 최대 대기 시간 (초)",
    )
    args = parser.parse_args()

    measure_delays(
        host=args.host,
        port=args.port,
        pin=args.pin,
        timeout_s=args.timeout,
    )


if __name__ == "__main__":
    main()

