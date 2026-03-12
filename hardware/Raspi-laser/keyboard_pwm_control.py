#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import time
from dataclasses import dataclass


SYSFS_PWM_CHIP = "/sys/class/pwm/pwmchip0"
SYSFS_PWM0 = f"{SYSFS_PWM_CHIP}/pwm0"  # GPIO12 (PWM0)
SYSFS_PWM1 = f"{SYSFS_PWM_CHIP}/pwm1"  # GPIO13 (PWM1)
GPIO_PWM0 = 12
GPIO_PWM1 = 13


def _write_text(path: str, text: str) -> None:
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def ensure_pwm_exported(period_ns: int) -> None:
    for ch in (0, 1):
        try:
            _write_text(f"{SYSFS_PWM_CHIP}/export", str(ch))
        except OSError:
            pass

    time.sleep(0.15)

    for base in (SYSFS_PWM0, SYSFS_PWM1):
        period_path = f"{base}/period"
        if os.path.exists(period_path):
            _write_text(period_path, str(int(period_ns)))

    time.sleep(0.05)

    for base in (SYSFS_PWM0, SYSFS_PWM1):
        enable_path = f"{base}/enable"
        if os.path.exists(enable_path):
            _write_text(enable_path, "1")


def set_pwm_us(channel: int, us: int) -> None:
    base = SYSFS_PWM0 if channel == 0 else SYSFS_PWM1
    duty_path = f"{base}/duty_cycle"
    _write_text(duty_path, str(int(us) * 1000))


def set_enable(channel: int, enabled: bool) -> None:
    base = SYSFS_PWM0 if channel == 0 else SYSFS_PWM1
    enable_path = f"{base}/enable"
    if os.path.exists(enable_path):
        _write_text(enable_path, "1" if enabled else "0")


def clamp(v: int, lo: int, hi: int) -> int:
    return lo if v < lo else hi if v > hi else v


@dataclass(frozen=True)
class DecoupleModel:
    """Local linear coupling model in world units per microsecond.

    [dx]   [dx/dp  dx/dt] [dp]
    [dy] ≈ [dy/dp  dy/dt] [dt]
    """

    dx_dp: float
    dx_dt: float
    dy_dp: float
    dy_dt: float

    def dt_for_dp_hold_y(self, dp_us: int) -> int:
        """Given dp, return dt that makes dy≈0."""
        if abs(self.dy_dt) < 1e-9:
            return 0
        return int(round(-(self.dy_dp / self.dy_dt) * float(dp_us)))

    def dp_for_dt_hold_x(self, dt_us: int) -> int:
        """Given dt, return dp that makes dx≈0."""
        if abs(self.dx_dp) < 1e-9:
            return 0
        return int(round(-(self.dx_dt / self.dx_dp) * float(dt_us)))


class PwmBackend:
    def start(self, args: argparse.Namespace, st: "State") -> None:
        raise NotImplementedError

    def set_pwm0_us(self, us: int) -> None:
        raise NotImplementedError

    def set_pwm1_us(self, us: int) -> None:
        raise NotImplementedError

    def stop(self) -> None:
        raise NotImplementedError


class SysfsBackend(PwmBackend):
    def __init__(self) -> None:
        self.enabled = False

    def start(self, args: argparse.Namespace, st: "State") -> None:
        if os.geteuid() != 0 and not args.no_pwm:
            raise SystemExit(
                "sysfs 백엔드는 보통 sudo가 필요합니다. 예) sudo python3 keyboard_pwm_control.py --backend sysfs"
            )
        if not os.path.exists(SYSFS_PWM_CHIP):
            raise SystemExit(
                f"{SYSFS_PWM_CHIP} 가 없습니다. pwm-2chan overlay 및 재부팅, 또는 커널 PWM 설정을 확인하세요."
            )
        ensure_pwm_exported(args.period_ns)
        set_pwm_us(0, st.pwm0_us)
        set_pwm_us(1, st.pwm1_us)
        self.enabled = True

    def set_pwm0_us(self, us: int) -> None:
        if self.enabled:
            set_pwm_us(0, us)

    def set_pwm1_us(self, us: int) -> None:
        if self.enabled:
            set_pwm_us(1, us)

    def stop(self) -> None:
        if not self.enabled:
            return
        try:
            set_enable(0, False)
            set_enable(1, False)
        except OSError:
            pass


class PigpioBackend(PwmBackend):
    def __init__(self, host: str, port: int) -> None:
        self.host = host
        self.port = int(port)
        self.pi = None

    def start(self, args: argparse.Namespace, st: "State") -> None:
        try:
            import pigpio  # type: ignore
        except Exception:
            raise SystemExit(
                "pigpio 모듈이 없습니다. 설치 예) sudo apt install -y pigpio python3-pigpio"
            )

        self.pi = pigpio.pi(self.host, self.port)
        if self.pi is None or not self.pi.connected:
            raise SystemExit(
                "pigpiod에 연결할 수 없습니다. 먼저 데몬을 실행하세요. 예) sudo systemctl enable --now pigpiod"
            )

        self.pi.set_mode(GPIO_PWM0, pigpio.OUTPUT)
        self.pi.set_mode(GPIO_PWM1, pigpio.OUTPUT)
        self.set_pwm0_us(st.pwm0_us)
        self.set_pwm1_us(st.pwm1_us)

    def set_pwm0_us(self, us: int) -> None:
        if self.pi is None:
            return
        self.pi.set_servo_pulsewidth(GPIO_PWM0, int(us))

    def set_pwm1_us(self, us: int) -> None:
        if self.pi is None:
            return
        self.pi.set_servo_pulsewidth(GPIO_PWM1, int(us))

    def stop(self) -> None:
        if self.pi is None:
            return
        try:
            self.pi.set_servo_pulsewidth(GPIO_PWM0, 0)
            self.pi.set_servo_pulsewidth(GPIO_PWM1, 0)
        finally:
            try:
                self.pi.stop()
            except Exception:
                pass


@dataclass
class State:
    pwm0_us: int
    pwm1_us: int
    dirty0: bool = True
    dirty1: bool = True
    decouple: bool = False


def run_tui(args: argparse.Namespace) -> int:
    import curses

    st = State(pwm0_us=int(args.init0_us), pwm1_us=int(args.init1_us))
    st.decouple = bool(getattr(args, "decouple", False))

    decouple_model = DecoupleModel(
        dx_dp=float(args.dx_dp),
        dx_dt=float(args.dx_dt),
        dy_dp=float(args.dy_dp),
        dy_dt=float(args.dy_dt),
    )

    backend: PwmBackend
    if args.no_pwm:
        backend = SysfsBackend()  # unused; only for type compatibility
    else:
        if args.backend == "pigpio":
            backend = PigpioBackend(args.pigpio_host, args.pigpio_port)
        else:
            backend = SysfsBackend()

    if not args.no_pwm:
        backend.start(args, st)
        st.dirty0 = False
        st.dirty1 = False

    def draw(scr: "curses._CursesWindow") -> None:
        scr.erase()
        mode = "DECOUPLE" if st.decouple else "NORMAL"
        scr.addstr(0, 0, f"GPIO12/13 PWM 키보드 제어 ({args.backend}) | mode={mode}")
        scr.addstr(2, 0, "조작:")
        scr.addstr(3, 2, f"← / → : PWM0 (GPIO12) {args.step_us}us 감소/증가")
        scr.addstr(4, 2, f"↑ / ↓ : PWM1 (GPIO13) {args.step_us}us 증가/감소")
        scr.addstr(5, 2, "m: 모드 변경(커플링 상쇄)    r: 리셋    q: 종료")
        if args.backend == "sysfs":
            scr.addstr(7, 0, f"범위: {args.min_us} ~ {args.max_us} us   period: {args.period_ns} ns (50Hz=20000000)")
        else:
            scr.addstr(7, 0, f"범위: {args.min_us} ~ {args.max_us} us   (pigpio servo pulsewidth, µs)")
        scr.addstr(9, 0, f"PWM0(GPIO12): {st.pwm0_us} us")
        scr.addstr(10, 0, f"PWM1(GPIO13): {st.pwm1_us} us")
        if st.decouple:
            k_dt_per_dp = 0.0 if abs(decouple_model.dy_dt) < 1e-9 else (-(decouple_model.dy_dp / decouple_model.dy_dt))
            k_dp_per_dt = 0.0 if abs(decouple_model.dx_dp) < 1e-9 else (-(decouple_model.dx_dt / decouple_model.dx_dp))
            scr.addstr(12, 0, f"상쇄계수: dt≈{k_dt_per_dp:+.3f}*dp (hold y), dp≈{k_dp_per_dt:+.3f}*dt (hold x)")
        if args.no_pwm:
            scr.addstr(14 if st.decouple else 12, 0, "no-pwm 모드: 실제 출력에 쓰지 않습니다.")
        scr.refresh()

    def apply_if_needed() -> None:
        if args.no_pwm:
            st.dirty0 = False
            st.dirty1 = False
            return
        if st.dirty0:
            backend.set_pwm0_us(st.pwm0_us)
            st.dirty0 = False
        if st.dirty1:
            backend.set_pwm1_us(st.pwm1_us)
            st.dirty1 = False

    try:
        curses.wrapper(lambda scr: _curses_loop(scr, args, st, decouple_model, draw, apply_if_needed))
    finally:
        if not args.no_pwm:
            backend.stop()
    return 0


def _curses_loop(scr, args, st: State, model: DecoupleModel, draw, apply_if_needed) -> None:
    import curses

    # 키보드 자동 반복(다다다다)로 입력이 버퍼에 쌓이면,
    # 키를 떼도 한동안 계속 움직이거나 튀는 것처럼 보일 수 있음.
    # nodelay + timeout을 두고, 매 틱마다 "누적 입력 중 마지막 키만" 반영해 백로그를 제거한다.
    scr.nodelay(True)
    scr.keypad(True)
    curses.curs_set(0)
    scr.timeout(int(getattr(args, "tick_ms", 30)))

    draw(scr)
    apply_if_needed()

    while True:
        k = scr.getch()
        if k == -1:
            continue

        # 같은 틱에 쌓인 입력이 있으면 전부 비우고 마지막 키만 사용
        last_k = k
        while True:
            k2 = scr.getch()
            if k2 == -1:
                break
            last_k = k2

        k = last_k
        if k in (ord("q"), ord("Q")):
            break
        if k in (ord("m"), ord("M")):
            # 모드 변경 시 "커플링 상쇄 모드"가 되도록: 토글 방식
            st.decouple = not st.decouple
            draw(scr)
            continue
        if k in (ord("r"), ord("R")):
            st.pwm0_us = int(args.init0_us)
            st.pwm1_us = int(args.init1_us)
            st.dirty0 = True
            st.dirty1 = True
        elif k == curses.KEY_LEFT:
            dp = -int(args.step_us)
            if st.decouple:
                dt = model.dt_for_dp_hold_y(dp)
                st.pwm1_us = clamp(st.pwm1_us + dt, int(args.min_us), int(args.max_us))
                st.dirty1 = True
            st.pwm0_us = clamp(st.pwm0_us + dp, int(args.min_us), int(args.max_us))
            st.dirty0 = True
        elif k == curses.KEY_RIGHT:
            dp = int(args.step_us)
            if st.decouple:
                dt = model.dt_for_dp_hold_y(dp)
                st.pwm1_us = clamp(st.pwm1_us + dt, int(args.min_us), int(args.max_us))
                st.dirty1 = True
            st.pwm0_us = clamp(st.pwm0_us + dp, int(args.min_us), int(args.max_us))
            st.dirty0 = True
        elif k == curses.KEY_UP:
            dt = int(args.step_us)
            if st.decouple:
                dp = model.dp_for_dt_hold_x(dt)
                st.pwm0_us = clamp(st.pwm0_us + dp, int(args.min_us), int(args.max_us))
                st.dirty0 = True
            st.pwm1_us = clamp(st.pwm1_us + dt, int(args.min_us), int(args.max_us))
            st.dirty1 = True
        elif k == curses.KEY_DOWN:
            dt = -int(args.step_us)
            if st.decouple:
                dp = model.dp_for_dt_hold_x(dt)
                st.pwm0_us = clamp(st.pwm0_us + dp, int(args.min_us), int(args.max_us))
                st.dirty0 = True
            st.pwm1_us = clamp(st.pwm1_us + dt, int(args.min_us), int(args.max_us))
            st.dirty1 = True

        if st.dirty0 or st.dirty1:
            apply_if_needed()
            draw(scr)

    # stop/cleanup은 run_tui()의 finally에서 처리


def main() -> int:
    p = argparse.ArgumentParser(description="GPIO12/13 (pwm0/pwm1) 키보드 화살표 제어")
    p.add_argument("--backend", choices=["pigpio", "sysfs"], default="pigpio", help="PWM 출력 백엔드")
    p.add_argument("--pigpio-host", default="localhost", help="pigpiod host")
    p.add_argument("--pigpio-port", type=int, default=8888, help="pigpiod port")
    p.add_argument("--init0-us", type=int, default=1290, help="초기 PWM0(GPIO12) 펄스폭(us)")
    p.add_argument("--init1-us", type=int, default=1390, help="초기 PWM1(GPIO13) 펄스폭(us)")
    p.add_argument("--step-us", type=int, default=10, help="키 입력 1회당 증감(us)")
    p.add_argument("--min-us", type=int, default=800, help="최소 펄스폭(us)")
    p.add_argument("--max-us", type=int, default=2200, help="최대 펄스폭(us)")
    p.add_argument("--period-ns", type=int, default=20_000_000, help="PWM period(ns), 50Hz=20000000")
    p.add_argument("--tick-ms", type=int, default=30, help="키 입력 처리 틱(ms). 입력 백로그/자동반복 완화용")
    p.add_argument("--decouple", action="store_true", help="커플링 상쇄 모드로 시작")
    # 커플링 모델 J (world units per µs). 기본값은 사용자 실측값 기반.
    # dx/dp=0.83, dx/dt=0.28, dy/dp=0.32, dy/dt=1.07  (cm/µs)
    p.add_argument("--dx-dp", type=float, default=0.83, help="dx/dp (예: cm/µs)")
    p.add_argument("--dx-dt", type=float, default=0.28, help="dx/dt (예: cm/µs)")
    p.add_argument("--dy-dp", type=float, default=0.32, help="dy/dp (예: cm/µs)")
    p.add_argument("--dy-dt", type=float, default=1.07, help="dy/dt (예: cm/µs)")
    p.add_argument("--no-pwm", action="store_true", help="실제 sysfs에 쓰지 않고 화면만 갱신")
    args = p.parse_args()
    return run_tui(args)


if __name__ == "__main__":
    raise SystemExit(main())
