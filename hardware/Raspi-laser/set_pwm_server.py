#!/usr/bin/env python3
"""
set_pwm_server.py — Raspberry Pi 상에서 TCP 서버로 동작.

Qt 클라이언트(PwmTransmitter)가 접속해서
  SET_PWM,PAN=1290,TILT=1390\n
형식으로 데이터를 보내면 GPIO 12/13 PWM 출력.

실행 예:
  python3 set_pwm_server.py --port 5566 -v
  python3 set_pwm_server.py --port 5566 --backend sysfs -v
"""
from __future__ import annotations

import argparse
import os
import socket
import sys
import threading
import time
from dataclasses import dataclass


GPIO_PWM0 = 12  # PAN  (pwm0)
GPIO_PWM1 = 13  # TILT (pwm1)

SYSFS_PWM_CHIP = "/sys/class/pwm/pwmchip0"
SYSFS_PWM0 = f"{SYSFS_PWM_CHIP}/pwm0"
SYSFS_PWM1 = f"{SYSFS_PWM_CHIP}/pwm1"


def clamp_int(v: int, lo: int, hi: int) -> int:
    return lo if v < lo else hi if v > hi else v


def parse_set_pwm(line: str) -> tuple[int, int] | None:
    """
    SET_PWM,PAN=1290,TILT=1390  형식 파싱
    """
    s = line.strip()
    if "SET_PWM" not in s:
        return None
    try:
        pan_i  = s.upper().find("PAN=")
        tilt_i = s.upper().find("TILT=")
        if pan_i < 0 or tilt_i < 0:
            return None
        pan_part  = s[pan_i + 4 :].split(",", 1)[0].strip()
        tilt_part = s[tilt_i + 5:].split(",", 1)[0].strip()
        return int(float(pan_part)), int(float(tilt_part))
    except Exception:
        return None


# ── PWM 출력 백엔드 ──────────────────────────────────────────────

class PwmOut:
    def start(self) -> None:   raise NotImplementedError
    def set_us(self, pan_us: int, tilt_us: int) -> None: raise NotImplementedError
    def stop(self) -> None:    raise NotImplementedError


class PigpioOut(PwmOut):
    def __init__(self, host: str, port: int) -> None:
        self.host = host
        self.port = int(port)
        self.pi   = None

    def start(self) -> None:
        try:
            import pigpio  # type: ignore
        except Exception:
            raise SystemExit("pigpio 모듈 없음. 설치: sudo apt install -y pigpio python3-pigpio")
        self.pi = pigpio.pi(self.host, self.port)
        if not self.pi.connected:
            raise SystemExit("pigpiod 연결 실패. 실행: sudo systemctl enable --now pigpiod")
        self.pi.set_mode(GPIO_PWM0, pigpio.OUTPUT)
        self.pi.set_mode(GPIO_PWM1, pigpio.OUTPUT)

    def set_us(self, pan_us: int, tilt_us: int) -> None:
        if self.pi:
            self.pi.set_servo_pulsewidth(GPIO_PWM0, pan_us)
            self.pi.set_servo_pulsewidth(GPIO_PWM1, tilt_us)

    def stop(self) -> None:
        if self.pi:
            try:
                self.pi.set_servo_pulsewidth(GPIO_PWM0, 0)
                self.pi.set_servo_pulsewidth(GPIO_PWM1, 0)
            finally:
                try: self.pi.stop()
                except Exception: pass


def _write_text(path: str, text: str) -> None:
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


class SysfsOut(PwmOut):
    def __init__(self, period_ns: int) -> None:
        self.period_ns = int(period_ns)
        self.enabled   = False

    def start(self) -> None:
        if os.geteuid() != 0:
            raise SystemExit("sysfs PWM은 sudo 필요. 예) sudo python3 set_pwm_server.py --backend sysfs")
        if not os.path.exists(SYSFS_PWM_CHIP):
            raise SystemExit(f"{SYSFS_PWM_CHIP} 없음. pwm-2chan overlay 및 재부팅 확인")
        for ch in (0, 1):
            try: _write_text(f"{SYSFS_PWM_CHIP}/export", str(ch))
            except OSError: pass
        time.sleep(0.15)
        for base in (SYSFS_PWM0, SYSFS_PWM1):
            if os.path.exists(f"{base}/period"):
                _write_text(f"{base}/period", str(self.period_ns))
        time.sleep(0.05)
        for base in (SYSFS_PWM0, SYSFS_PWM1):
            if os.path.exists(f"{base}/enable"):
                _write_text(f"{base}/enable", "1")
        self.enabled = True

    def set_us(self, pan_us: int, tilt_us: int) -> None:
        if self.enabled:
            _write_text(f"{SYSFS_PWM0}/duty_cycle", str(pan_us  * 1000))
            _write_text(f"{SYSFS_PWM1}/duty_cycle", str(tilt_us * 1000))

    def stop(self) -> None:
        if self.enabled:
            for base in (SYSFS_PWM0, SYSFS_PWM1):
                try:
                    if os.path.exists(f"{base}/enable"):
                        _write_text(f"{base}/enable", "0")
                except OSError: pass


# ── 클라이언트 핸들러 ──────────────────────────────────────────

def handle_client(conn: socket.socket, addr, cfg, out: PwmOut) -> None:
    if cfg.verbose:
        sys.stderr.write(f"[set_pwm_server] Qt client connected: {addr}\n")
    buf = b""
    try:
        while True:
            data = conn.recv(512)
            if not data:
                break
            buf += data
            while b"\n" in buf:
                line_b, _, buf = buf.partition(b"\n")
                line = line_b.strip().decode("utf-8", errors="ignore")
                if not line:
                    continue
                m = parse_set_pwm(line)
                if m is None:
                    continue
                pan, tilt = m
                pan  = clamp_int(pan,  cfg.min_us, cfg.max_us)
                tilt = clamp_int(tilt, cfg.min_us, cfg.max_us)
                out.set_us(pan, tilt)
                if cfg.verbose:
                    sys.stderr.write(f"[set_pwm_server] PAN={pan} TILT={tilt}\n")
    except Exception as e:
        if cfg.verbose:
            sys.stderr.write(f"[set_pwm_server] client error: {e}\n")
    finally:
        try: conn.close()
        except Exception: pass
        if cfg.verbose:
            sys.stderr.write(f"[set_pwm_server] Qt client disconnected: {addr}\n")


# ── 서버 메인 루프 ─────────────────────────────────────────────

def serve(cfg, out: PwmOut) -> None:
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", cfg.port))
    server.listen(4)
    sys.stderr.write(f"[set_pwm_server] Listening on 0.0.0.0:{cfg.port} ...\n")
    sys.stderr.write(f"[set_pwm_server] Qt 설정: SFEPS_PWM_HOST=<RaspberryPi_IP>  SFEPS_PWM_PORT={cfg.port}\n")

    while True:
        try:
            conn, addr = server.accept()
        except KeyboardInterrupt:
            break
        t = threading.Thread(target=handle_client, args=(conn, addr, cfg, out), daemon=True)
        t.start()


@dataclass
class Config:
    port: int
    backend: str
    pigpio_host: str
    pigpio_port: int
    period_ns: int
    min_us: int
    max_us: int
    verbose: bool


def main() -> int:
    p = argparse.ArgumentParser(
        description="Raspberry Pi TCP 서버 — Qt PwmTransmitter로부터 SET_PWM 수신 후 GPIO PWM 출력")
    p.add_argument("--port",        type=int, default=5566,  help="리슨 포트 (기본 5566)")
    p.add_argument("--backend",     choices=["pigpio", "sysfs"], default="pigpio")
    p.add_argument("--pigpio-host", default="localhost")
    p.add_argument("--pigpio-port", type=int, default=8888)
    p.add_argument("--period-ns",   type=int, default=20_000_000)
    p.add_argument("--min-us",      type=int, default=800)
    p.add_argument("--max-us",      type=int, default=2200)
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args()

    cfg = Config(
        port=args.port,
        backend=args.backend,
        pigpio_host=args.pigpio_host,
        pigpio_port=args.pigpio_port,
        period_ns=args.period_ns,
        min_us=args.min_us,
        max_us=args.max_us,
        verbose=args.verbose,
    )

    out: PwmOut = PigpioOut(cfg.pigpio_host, cfg.pigpio_port) \
                  if cfg.backend == "pigpio" else SysfsOut(cfg.period_ns)
    out.start()
    try:
        serve(cfg, out)
    except KeyboardInterrupt:
        pass
    finally:
        out.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
