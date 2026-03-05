#!/usr/bin/env python3
"""
Raspi-laser PID + PWM 에이전트 (50Hz 제어 주기).

- Ubuntu TCP 서버(host 파이프라인)에서 전송하는 "EX=float,EY=float\\n" 오차를 수신.
- 20ms(50Hz) 주기로 PID 연산 후 GPIO12(pwm0)/GPIO13(pwm1) PWM 듀티 갱신.
- 초기 PWM: x=1530µs, y=1300µs (README 고정값).
- PWM 범위: 800~2200µs (서보/레이저 구동 공통).

사용 예:
  python3 pid_pwm_agent.py --host 10.42.0.1 --port 5555
  python3 pid_pwm_agent.py --host 10.42.0.1 --port 5555 --kp-x 0.02 --kp-y -0.02
"""

from __future__ import annotations

import argparse
import re
import socket
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Optional, TextIO, List, Tuple

# -----------------------------------------------------------------------------
# 상수 (README 및 STM32 호스트와 맞춤)
# -----------------------------------------------------------------------------
PWM_PERIOD_NS = 20_000_000   # 50Hz
PWM_MIN_US = 800
PWM_MAX_US = 2200
INIT_X_US = 1530  # pwm0 (GPIO12) 초기값
INIT_Y_US = 1300  # pwm1 (GPIO13) 초기값
CONTROL_PERIOD_S = 0.02      # 50Hz = 20ms

# Ubuntu 서버가 보내는 형식
EXEY_RE = re.compile(r"EX=([-\d.]+),EY=([-\d.]+)")

# Linux sysfs PWM (pwmchip0 = bcm2835)
SYSFS_PWM_CHIP = "/sys/class/pwm/pwmchip0"
SYSFS_PWM0 = f"{SYSFS_PWM_CHIP}/pwm0"
SYSFS_PWM1 = f"{SYSFS_PWM_CHIP}/pwm1"

# 선택적: auto_tune_pid 재사용 (numpy/scipy/control 필요)
try:
    from pathlib import Path as _Path
    _ROOT = _Path(__file__).resolve().parents[2]
    if str(_ROOT / "hardware/stm32-laser/PID_control") not in sys.path:
        sys.path.append(str(_ROOT / "hardware/stm32-laser/PID_control"))
    import auto_tune_pid as _at  # type: ignore
    HAVE_AUTOTUNE = True
except Exception:
    _at = None
    HAVE_AUTOTUNE = False


@dataclass
class PidState:
    """위치형 PID.
    u = center_us + Kp*e + Ki*∫e*dt + Kd*de/dt
    - center_us: 초기 PWM 기준값. Ki가 center와 실제 "에러=0 위치" 사이의 차이를 보정.
    - 서보는 위치 액추에이터이므로 위치형이 자연스러움.
    - 50Hz 매 틱마다 호출해야 Ki가 제대로 쌓임.
    """
    kp: float
    ki: float
    kd: float
    center_us: float
    integral: float = 0.0
    last_error: float = 0.0

    def update(self, error: float, dt: float) -> float:
        if dt <= 0:
            return self.center_us + self.kp * error
        self.integral += error * dt
        self.integral = max(-2000.0, min(2000.0, self.integral))
        deriv = (error - self.last_error) / dt
        self.last_error = error
        out_us = self.center_us + self.kp * error + self.ki * self.integral + self.kd * deriv
        return float(max(PWM_MIN_US, min(PWM_MAX_US, out_us)))


def us_to_ns(us: float) -> int:
    return int(round(us * 1000.0))


def write_pwm_duty(channel: int, duty_ns: int) -> None:
    """channel 0 = pwm0 (x), 1 = pwm1 (y). duty_ns in nanoseconds. (실행 시 sudo 필요.)"""
    path = SYSFS_PWM0 if channel == 0 else SYSFS_PWM1
    file = f"{path}/duty_cycle"
    with open(file, "w") as f:
        f.write(str(duty_ns))


def ensure_pwm_exported() -> None:
    """pwm0, pwm1 export 후 period=20ms, enable=1 설정. (실행 시 sudo 필요.)"""
    import os
    for ch in (0, 1):
        export_path = f"{SYSFS_PWM_CHIP}/export"
        try:
            with open(export_path, "w") as f:
                f.write(str(ch))
        except (OSError, FileNotFoundError):
            pass
    time.sleep(0.15)
    for ch in (0, 1):
        p = SYSFS_PWM0 if ch == 0 else SYSFS_PWM1
        period_path = f"{p}/period"
        if not os.path.exists(period_path):
            sys.stderr.write(f"[pid_pwm_agent] Warning: {period_path} not found. Run with sudo.\n")
            continue
        with open(period_path, "w") as f:
            f.write(str(PWM_PERIOD_NS))
    time.sleep(0.05)
    for ch in (0, 1):
        p = SYSFS_PWM0 if ch == 0 else SYSFS_PWM1
        enable_path = f"{p}/enable"
        if os.path.exists(enable_path):
            with open(enable_path, "w") as f:
                f.write("1")


@dataclass
class SharedState:
    """수신 스레드와 제어 루프가 공유하는 최신 오차."""
    ex: float = 0.0
    ey: float = 0.0
    updated: bool = False
    lock: threading.Lock = field(default_factory=threading.Lock)


def recv_loop(sock: socket.socket, shared: SharedState) -> None:
    """TCP로 한 줄씩 읽어 EX=...,EY=... 파싱 후 shared 갱신."""
    buf = b""
    while True:
        try:
            data = sock.recv(256)
            if not data:
                break  # 서버가 소켓을 닫은 경우
            buf += data
            while b"\n" in buf or b"\r" in buf:
                line, _, buf = buf.partition(b"\n")
                line = line.strip().decode("utf-8", errors="ignore").strip()
                if not line:
                    continue
                m = EXEY_RE.search(line)
                if m:
                    try:
                        ex = float(m.group(1))
                        ey = float(m.group(2))
                        with shared.lock:
                            shared.ex = ex
                            shared.ey = ey
                            shared.updated = True
                    except ValueError:
                        pass
        except socket.timeout:
            # 타임아웃은 정상: 그냥 다시 recv 시도
            continue
        except (ConnectionResetError, BrokenPipeError, OSError):
            break
    sys.stderr.write("[pid_pwm_agent] Recv loop ended.\n")


def main() -> None:
    parser = argparse.ArgumentParser(description="Raspi PID+PWM agent (50Hz), receives EX,EY from Ubuntu TCP server")
    parser.add_argument("--host", default="10.42.0.1", help="Ubuntu TCP server IP (e.g. notebook AP gateway)")
    parser.add_argument("--port", type=int, default=5555, help="TCP port")
    parser.add_argument("--kp-x", type=float, default=0.02, help="P gain for X (pan) axis")
    parser.add_argument("--ki-x", type=float, default=0.0)
    parser.add_argument("--kd-x", type=float, default=0.0)
    parser.add_argument("--kp-y", type=float, default=-0.02, help="P gain for Y (tilt), typically negative")
    parser.add_argument("--ki-y", type=float, default=0.0)
    parser.add_argument("--kd-y", type=float, default=0.0)
    parser.add_argument("--init-x-us", type=float, default=INIT_X_US, help="Initial PWM x (us)")
    parser.add_argument("--init-y-us", type=float, default=INIT_Y_US, help="Initial PWM y (us)")
    parser.add_argument("--no-pwm", action="store_true", help="Do not touch sysfs PWM (dry run)")
    parser.add_argument(
        "--log-file",
        help="STM32 auto_tune_pid.py 와 호환되는 PIDLOG 포맷으로 로그를 남길 파일 경로",
    )
    parser.add_argument(
        "--auto-tune-samples",
        type=int,
        default=0,
        help=">0 이면 이 샘플 수만큼 (EX,EY,UX,UY) 를 모은 뒤 auto_tune_pid 알고리즘으로 한 번 자동 튜닝",
    )
    args = parser.parse_args()

    shared = SharedState()

    log_f: Optional[TextIO] = None
    if args.log_file:
        try:
            log_f = open(args.log_file, "a", buffering=1, encoding="utf-8")
        except OSError as e:
            sys.stderr.write(f"[pid_pwm_agent] Failed to open log file {args.log_file}: {e}\n")
            log_f = None

    # TCP 클라이언트: Ubuntu 서버에 접속
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10.0)
    try:
        sock.connect((args.host, args.port))
    except (socket.error, OSError) as e:
        sys.stderr.write(f"[pid_pwm_agent] Connect failed: {e}\n")
        sys.exit(1)
    sock.settimeout(0.5)

    if not args.no_pwm:
        ensure_pwm_exported()
        # 초기 듀티 설정
        write_pwm_duty(0, us_to_ns(args.init_x_us))
        write_pwm_duty(1, us_to_ns(args.init_y_us))
    else:
        sys.stderr.write("[pid_pwm_agent] Dry run: PWM sysfs not written.\n")

    pid_x = PidState(kp=args.kp_x, ki=args.ki_x, kd=args.kd_x, center_us=args.init_x_us)
    pid_y = PidState(kp=args.kp_y, ki=args.ki_y, kd=args.kd_y, center_us=args.init_y_us)

    samples_x: List[Tuple[int, float, float]] = []  # (t_ms, ex, ux_us)
    samples_y: List[Tuple[int, float, float]] = []  # (t_ms, ey, uy_us)
    auto_tuned = False

    th = threading.Thread(target=recv_loop, args=(sock, shared), daemon=True)
    th.start()

    t0 = time.monotonic()
    next_control = t0
    log_count = 0
    ux_us = args.init_x_us
    uy_us = args.init_y_us

    try:
        while True:
            now = time.monotonic()
            if now >= next_control:
                next_control += CONTROL_PERIOD_S
                dt = CONTROL_PERIOD_S

                with shared.lock:
                    ex = shared.ex
                    ey = shared.ey
                    had_update = shared.updated
                    shared.updated = False

                # 매 틱(50Hz)마다 PID 업데이트 — Ki 적분이 매 틱 쌓여야 정상상태 오차 제거 가능
                ux_us = pid_x.update(ex, dt)
                uy_us = pid_y.update(ey, dt)

                if not args.no_pwm:
                    write_pwm_duty(0, us_to_ns(ux_us))
                    write_pwm_duty(1, us_to_ns(uy_us))

                # 샘플 버퍼에 누적 (온라인 튜닝용, 새 데이터 들어온 틱만)
                if had_update and args.auto_tune_samples > 0 and not auto_tuned:
                    t_ms = int((time.monotonic() - t0) * 1000.0)
                    samples_x.append((t_ms, ex, ux_us))
                    samples_y.append((t_ms, ey, uy_us))

                    if (
                        HAVE_AUTOTUNE
                        and len(samples_x) >= args.auto_tune_samples
                        and len(samples_y) >= args.auto_tune_samples
                    ):
                        try:
                            import numpy as _np

                            t_arr = _np.array([s[0] for s in samples_x], dtype=float)
                            if len(t_arr) > 1:
                                ts = float(_np.median(_np.diff(t_arr)) / 1000.0)
                            else:
                                ts = CONTROL_PERIOD_S

                            ex_arr = _np.array([s[1] for s in samples_x], dtype=float)
                            ux_arr = _np.array([s[2] for s in samples_x], dtype=float)
                            ey_arr = _np.array([s[1] for s in samples_y], dtype=float)
                            uy_arr = _np.array([s[2] for s in samples_y], dtype=float)

                            init_kpx = abs(pid_x.kp) if pid_x.kp != 0.0 else 0.01
                            init_kpy = abs(pid_y.kp) if pid_y.kp != 0.0 else 0.01

                            rx = _at.tune_axis(ts, ex_arr, ux_arr, init_kpx)
                            ry = _at.tune_axis(ts, ey_arr, uy_arr, init_kpy)

                            pid_x.kp, pid_x.ki, pid_x.kd = rx.kp, rx.ki, rx.kd
                            pid_y.kp, pid_y.ki, pid_y.kd = -ry.kp, -ry.ki, -ry.kd
                            auto_tuned = True

                            sys.stderr.write(
                                "[pid_pwm_agent] Auto-tune complete.\n"
                                f"  X: kp={pid_x.kp:.4f}, ki={pid_x.ki:.4f}, kd={pid_x.kd:.4f}\n"
                                f"  Y: kp={pid_y.kp:.4f}, ki={pid_y.ki:.4f}, kd={pid_y.kd:.4f}\n"
                            )
                        except Exception as e:
                            sys.stderr.write(f"[pid_pwm_agent] Auto-tune failed: {e}\n")
                            auto_tuned = True

                # PIDLOG 로그 (새 데이터 들어온 틱만 기록)
                if had_update and log_f is not None:
                    t_ms = int((time.monotonic() - t0) * 1000.0)
                    log_f.write(
                        "PIDLOG,"
                        f"t:{t_ms},"
                        f"ex:{ex:.4f},ey:{ey:.4f},"
                        f"out_x:{ux_us:.0f},out_y:{uy_us:.0f},"
                        f"kpx:{pid_x.kp:.4f},kix:{pid_x.ki:.4f},kdx:{pid_x.kd:.4f},"
                        f"kpy:{pid_y.kp:.4f},kiy:{pid_y.ki:.4f},kdy:{pid_y.kd:.4f}\n"
                    )

                log_count += 1
                if log_count % 50 == 0:
                    if had_update:
                        sys.stderr.write(
                            f"[pid_pwm_agent] ex={ex:.2f} ey={ey:.2f} -> ux={ux_us:.0f} us uy={uy_us:.0f} us\n"
                        )
                    else:
                        sys.stderr.write(
                            f"[pid_pwm_agent] (no new EX/EY) hold ux={ux_us:.0f} us uy={uy_us:.0f} us\n"
                        )

            time.sleep(0.005)
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()
        if log_f is not None:
            log_f.close()


if __name__ == "__main__":
    main()
