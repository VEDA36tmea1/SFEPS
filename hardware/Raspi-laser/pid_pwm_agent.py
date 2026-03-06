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
# EX=...,EY=...[,TU=...,TV=...[,GR=...,GC=...]]
EXEY_RE = re.compile(
    r"EX=([-\d.]+),EY=([-\d.]+)"
    r"(?:,TU=([-\d.]+),TV=([-\d.]+)(?:,GR=(\d+),GC=(\d+))?)?"
)

# 서버 LUT: 호스트가 PWM 값 요청
REQUEST_PWM_RE = re.compile(r"REQUEST_PWM,GR=(\d+),GC=(\d+)")

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


class LutCollector:
    """그리드 기반 LUT 수집기.

    - 그리드 셀마다(=포인트마다) 즉시 파일로 저장(진행형 저장)
    - 저장 조건: |ex|<=th_ex && |ey|<=th_ey 를 stable_ticks 연속 만족
    - 동일 셀 재측정 시, 타겟이 그리드 중앙에 더 가까우면 덮어씀
    - seq=True 이면 (0,0) → (0,1) → ... → (rows-1, cols-1) 순서로만 저장
    """

    def __init__(
        self,
        total_rows: int = 11,
        total_cols: int = 19,
        th_ex: float = 10.0,
        th_ey: float = 10.0,
        stable_ticks: int = 15,
        out_path: str = "lut_data.json",
        seq: bool = True,
        frame_w: int = 1920,
        frame_h: int = 1080,
    ) -> None:
        self.total_rows = total_rows
        self.total_cols = total_cols
        self.total_points = total_rows * total_cols
        self.th_ex = th_ex
        self.th_ey = th_ey
        self.stable_ticks = stable_ticks
        self.out_path = out_path
        self.seq = seq
        self.frame_w = frame_w
        self.frame_h = frame_h

        self.enabled = False
        self.data: List[dict] = []
        self._collected: set[Tuple[int, int]] = set()
        self._best_dist: dict[Tuple[int, int], float] = {}
        self._cell_idx: dict[Tuple[int, int], int] = {}
        self._expected_idx = 0
        self._stable_count = 0
        self._last_key: Optional[Tuple[int, int]] = None

    def _grid_center_px(self, r: int, c: int) -> Tuple[float, float]:
        cx = (c + 0.5) * self.frame_w / self.total_cols
        cy = (r + 0.5) * self.frame_h / self.total_rows
        return cx, cy

    def _dist_to_center(self, grid_r: int, grid_c: int,
                        target_u: float, target_v: float) -> float:
        cx, cy = self._grid_center_px(grid_r, grid_c)
        return ((target_u - cx) ** 2 + (target_v - cy) ** 2) ** 0.5

    def start(self) -> None:
        self.enabled = True
        self.data.clear()
        self._collected.clear()
        self._best_dist.clear()
        self._cell_idx.clear()
        self._expected_idx = 0
        self._stable_count = 0
        self._last_key = None
        self._save()
        print(
            f"[LUT] 수집 시작 {self.total_rows}x{self.total_cols}={self.total_points} | "
            f"조건: |ex|<={self.th_ex:g}, |ey|<={self.th_ey:g} 를 {self.stable_ticks}틱 연속 | "
            f"seq={self.seq} | out={self.out_path}"
        )

    def _expected_key(self) -> Tuple[int, int]:
        r = self._expected_idx // self.total_cols
        c = self._expected_idx % self.total_cols
        return (r, c)

    def update(
        self,
        grid_r: int,
        grid_c: int,
        target_u: float,
        target_v: float,
        ex: float,
        ey: float,
        pan_us: float,
        tilt_us: float,
    ) -> bool:
        if not self.enabled:
            return False
        if grid_r < 0 or grid_c < 0:
            return False

        key = (grid_r, grid_c)
        if self.seq and key != self._expected_key():
            if self._last_key != key:
                self._last_key = key
                self._stable_count = 0
            return False

        dist = self._dist_to_center(grid_r, grid_c, target_u, target_v)
        is_upgrade = False

        if key in self._collected:
            if dist < self._best_dist.get(key, float("inf")):
                is_upgrade = True
            else:
                return False

        if self._last_key != key:
            self._last_key = key
            self._stable_count = 0

        if abs(ex) <= self.th_ex and abs(ey) <= self.th_ey:
            self._stable_count += 1
        else:
            self._stable_count = 0

        if self._stable_count < self.stable_ticks:
            return False

        self._stable_count = 0
        entry = {
            "grid_r": int(grid_r),
            "grid_c": int(grid_c),
            "target_u": float(target_u),
            "target_v": float(target_v),
            "ex": float(ex),
            "ey": float(ey),
            "pan_us": float(pan_us),
            "tilt_us": float(tilt_us),
        }

        if is_upgrade:
            idx = self._cell_idx[key]
            old = self._best_dist[key]
            self.data[idx] = entry
            self._best_dist[key] = dist
            self._save()
            print(
                f"[LUT] updated cell=({grid_r},{grid_c}) "
                f"dist {old:.1f}→{dist:.1f} ex={ex:.1f} ey={ey:.1f} "
                f"→ pan={pan_us:.0f} tilt={tilt_us:.0f}"
            )
        else:
            self._collected.add(key)
            self._cell_idx[key] = len(self.data)
            self._best_dist[key] = dist
            self.data.append(entry)
            if self.seq:
                self._expected_idx += 1
            self._save()
            n = len(set(e["grid_r"] * self.total_cols + e["grid_c"] for e in self.data))
            print(
                f"[LUT] saved {n}/{self.total_points} "
                f"cell=({grid_r},{grid_c}) dist={dist:.1f} ex={ex:.1f} ey={ey:.1f} "
                f"→ pan={pan_us:.0f} tilt={tilt_us:.0f}"
            )

        unique_cells = len(self._collected)
        if unique_cells >= self.total_points:
            self.enabled = False
            print("[LUT] 모든 포인트 수집 완료")
            return True

        if self.seq and self.enabled:
            nr, nc = self._expected_key()
            print(f"[LUT] next cell: ({nr},{nc})")

        return True

    def _save(self) -> None:
        import json

        payload = {
            "rows": self.total_rows,
            "cols": self.total_cols,
            "seq": self.seq,
            "th_ex": self.th_ex,
            "th_ey": self.th_ey,
            "stable_ticks": self.stable_ticks,
            "count": len(self._collected),
            "total": self.total_points,
            "points": self.data,
            "timestamp": time.strftime("%Y%m%d_%H%M%S"),
        }
        with open(self.out_path, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2, ensure_ascii=False)


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
    """수신 스레드와 제어 루프가 공유하는 최신 오차 + LUT용 메타데이터."""
    ex: float = 0.0
    ey: float = 0.0
    target_u: float = 0.0
    target_v: float = 0.0
    grid_r: int = -1
    grid_c: int = -1
    updated: bool = False
    last_ux_us: float = INIT_X_US
    last_uy_us: float = INIT_Y_US
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
                # REQUEST_PWM: 현재 PWM 값 응답 (서버 LUT 저장용)
                m_req = REQUEST_PWM_RE.search(line)
                if m_req:
                    with shared.lock:
                        ux = shared.last_ux_us
                        uy = shared.last_uy_us
                    resp = f"PAN={ux:.0f},TILT={uy:.0f}\n"
                    try:
                        sock.sendall(resp.encode("utf-8"))
                        sys.stderr.write(f"[pid_pwm_agent] REQUEST_PWM → {resp.strip()}\n")
                    except OSError as e:
                        sys.stderr.write(f"[pid_pwm_agent] PWM 응답 전송 실패: {e}\n")
                    continue
                m = EXEY_RE.search(line)
                if m:
                    try:
                        ex = float(m.group(1))
                        ey = float(m.group(2))
                        tu = m.group(3)
                        tv = m.group(4)
                        gr = m.group(5)
                        gc = m.group(6)
                        with shared.lock:
                            shared.ex = ex
                            shared.ey = ey
                            if tu is not None and tv is not None:
                                shared.target_u = float(tu)
                                shared.target_v = float(tv)
                            if gr is not None and gc is not None:
                                shared.grid_r = int(gr)
                                shared.grid_c = int(gc)
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
    # 기본 게인(요청값 기준)
    parser.add_argument("--kp-x", type=float, default=0.25, help="P gain for X (pan) axis")
    parser.add_argument("--ki-x", type=float, default=0.3)
    parser.add_argument("--kd-x", type=float, default=0.0)
    parser.add_argument("--kp-y", type=float, default=-0.23, help="P gain for Y (tilt), typically negative")
    parser.add_argument("--ki-y", type=float, default=-0.5)
    parser.add_argument("--kd-y", type=float, default=0.0)
    parser.add_argument("--init-x-us", type=float, default=INIT_X_US, help="Initial PWM x (us)")
    parser.add_argument("--init-y-us", type=float, default=INIT_Y_US, help="Initial PWM y (us)")
    parser.add_argument("--no-pwm", action="store_true", help="Do not touch sysfs PWM (dry run)")
    parser.add_argument(
        "--log-file",
        help="STM32 auto_tune_pid.py 와 호환되는 PIDLOG 포맷으로 로그를 남길 파일 경로",
    )
    # 기존 커맨드 호환: --log 를 --log-file 별칭으로 지원
    parser.add_argument(
        "--log",
        dest="log_file",
        help="(--log-file alias) PIDLOG 포맷 로그 파일 경로",
    )
    parser.add_argument(
        "--auto-tune-samples",
        type=int,
        default=0,
        help=">0 이면 이 샘플 수만큼 (EX,EY,UX,UY) 를 모은 뒤 auto_tune_pid 알고리즘으로 한 번 자동 튜닝",
    )
    parser.add_argument(
        "--lut-mode",
        action="store_true",
        help="11x19 화면 그리드 LUT 수집 모드 활성화",
    )
    parser.add_argument("--lut-rows", type=int, default=11)
    parser.add_argument("--lut-cols", type=int, default=19)
    parser.add_argument("--lut-th-ex", type=float, default=7.0, help="LUT 저장 조건 |ex| <= th")
    parser.add_argument("--lut-th-ey", type=float, default=7.0, help="LUT 저장 조건 |ey| <= th")
    parser.add_argument("--lut-stable-ticks", type=int, default=5, help="오차 조건 연속 만족 틱 수 (5=0.1초)")
    parser.add_argument("--lut-out", default="lut_data.json", help="진행형 LUT 저장 파일(덮어씀)")
    parser.add_argument("--lut-seq", action="store_true", help="그리드 순서 강제 (기본: 어떤 셀이든 저장)")
    parser.add_argument("--frame-w", type=int, default=1920, help="카메라 해상도 가로 (그리드 중앙 계산용)")
    parser.add_argument("--frame-h", type=int, default=1080, help="카메라 해상도 세로 (그리드 중앙 계산용)")
    args = parser.parse_args()

    shared = SharedState()
    collector = LutCollector(
        total_rows=args.lut_rows,
        total_cols=args.lut_cols,
        th_ex=args.lut_th_ex,
        th_ey=args.lut_th_ey,
        stable_ticks=args.lut_stable_ticks,
        out_path=args.lut_out,
        seq=args.lut_seq,
        frame_w=args.frame_w,
        frame_h=args.frame_h,
    )
    if args.lut_mode:
        collector.start()
        sys.stderr.write("[pid_pwm_agent] [LUT] 수집 모드 ON\n")

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
                    tu = shared.target_u
                    tv = shared.target_v
                    gr = shared.grid_r
                    gc = shared.grid_c
                    had_update = shared.updated
                    shared.updated = False

                ux_us = pid_x.update(ex, dt)
                uy_us = pid_y.update(ey, dt)

                if not args.no_pwm:
                    write_pwm_duty(0, us_to_ns(ux_us))
                    write_pwm_duty(1, us_to_ns(uy_us))

                # 서버 LUT: REQUEST_PWM 응답용 현재 PWM 갱신
                with shared.lock:
                    shared.last_ux_us = ux_us
                    shared.last_uy_us = uy_us

                if args.lut_mode and collector.enabled:
                    saved = collector.update(
                        grid_r=gr,
                        grid_c=gc,
                        target_u=tu,
                        target_v=tv,
                        ex=ex,
                        ey=ey,
                        pan_us=ux_us,
                        tilt_us=uy_us,
                    )
                    if saved:
                        ack_msg = f"SAVED,GR={gr},GC={gc}\n"
                        try:
                            sock.sendall(ack_msg.encode("utf-8"))
                            sys.stderr.write(f"[pid_pwm_agent] ACK sent: {ack_msg.strip()}\n")
                        except OSError as _e:
                            sys.stderr.write(f"[pid_pwm_agent] ACK send failed: {_e}\n")

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
