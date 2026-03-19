#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import socket
import sys
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
    Accepts e.g.
      SET_PWM,PAN=1290,TILT=1390
      SET_PWM,?PAN=1290,?TILT=1390
    Returns (pan_us, tilt_us) as int if parsable.
    """
    s = line.strip()
    if "SET_PWM" not in s:
        return None
    # cheap parse: find PAN= and TILT=
    try:
        pan_i = s.upper().find("PAN=")
        tilt_i = s.upper().find("TILT=")
        if pan_i < 0 or tilt_i < 0:
            return None
        pan_part = s[pan_i + 4 :].split(",", 1)[0].strip()
        tilt_part = s[tilt_i + 5 :].split(",", 1)[0].strip()
        pan = int(float(pan_part))
        tilt = int(float(tilt_part))
        return pan, tilt
    except Exception:
        return None


class PwmOut:
    def start(self) -> None:
        raise NotImplementedError

    def set_us(self, pan_us: int, tilt_us: int) -> None:
        raise NotImplementedError

    def stop(self) -> None:
        raise NotImplementedError


class PigpioOut(PwmOut):
    def __init__(self, host: str, port: int) -> None:
        self.host = host
        self.port = int(port)
        self.pi = None

    def start(self) -> None:
        try:
            import pigpio  # type: ignore
        except Exception:
            raise SystemExit("pigpio 모듈이 없습니다. 설치: sudo apt install -y pigpio python3-pigpio")

        self.pi = pigpio.pi(self.host, self.port)
        if self.pi is None or not self.pi.connected:
            raise SystemExit("pigpiod 연결 실패. 실행: sudo systemctl enable --now pigpiod")

        self.pi.set_mode(GPIO_PWM0, pigpio.OUTPUT)
        self.pi.set_mode(GPIO_PWM1, pigpio.OUTPUT)

    def set_us(self, pan_us: int, tilt_us: int) -> None:
        if self.pi is None:
            return
        self.pi.set_servo_pulsewidth(GPIO_PWM0, int(pan_us))
        self.pi.set_servo_pulsewidth(GPIO_PWM1, int(tilt_us))

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


def _write_text(path: str, text: str) -> None:
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


class SysfsOut(PwmOut):
    def __init__(self, period_ns: int) -> None:
        self.period_ns = int(period_ns)
        self.enabled = False

    def start(self) -> None:
        if os.geteuid() != 0:
            raise SystemExit("sysfs PWM은 보통 sudo가 필요합니다. 예) sudo python3 set_pwm_client.py --backend sysfs")
        if not os.path.exists(SYSFS_PWM_CHIP):
            raise SystemExit(f"{SYSFS_PWM_CHIP} 가 없습니다. pwm-2chan overlay 및 재부팅 여부 확인")

        for ch in (0, 1):
            try:
                _write_text(f"{SYSFS_PWM_CHIP}/export", str(ch))
            except OSError:
                pass
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
        if not self.enabled:
            return
        _write_text(f"{SYSFS_PWM0}/duty_cycle", str(int(pan_us) * 1000))
        _write_text(f"{SYSFS_PWM1}/duty_cycle", str(int(tilt_us) * 1000))

    def stop(self) -> None:
        if not self.enabled:
            return
        for base in (SYSFS_PWM0, SYSFS_PWM1):
            try:
                if os.path.exists(f"{base}/enable"):
                    _write_text(f"{base}/enable", "0")
            except OSError:
                pass


@dataclass
class Config:
    host: str
    port: int
    backend: str
    pigpio_host: str
    pigpio_port: int
    period_ns: int
    min_us: int
    max_us: int
    verbose: bool


def connect_loop(cfg: Config, out: PwmOut) -> None:
    backoff_s = 0.2
    while True:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5.0)
        try:
            sock.connect((cfg.host, cfg.port))
            sock.settimeout(1.0)
            if cfg.verbose:
                sys.stderr.write(f"[set_pwm_client] connected to {cfg.host}:{cfg.port}\n")
            backoff_s = 0.2
            recv_lines(sock, cfg, out)
        except KeyboardInterrupt:
            raise
        except Exception as e:
            if cfg.verbose:
                sys.stderr.write(f"[set_pwm_client] connect/run error: {e}\n")
        finally:
            try:
                sock.close()
            except Exception:
                pass

        time.sleep(backoff_s)
        backoff_s = min(5.0, backoff_s * 1.6)


def recv_lines(sock: socket.socket, cfg: Config, out: PwmOut) -> None:
    buf = b""
    while True:
        try:
            data = sock.recv(512)
            if not data:
                return
            buf += data
            while b"\n" in buf or b"\r" in buf:
                line_b, _, buf = buf.partition(b"\n")
                line = line_b.strip().decode("utf-8", errors="ignore").strip()
                if not line:
                    continue
                m = parse_set_pwm(line)
                if m is None:
                    continue
                pan, tilt = m
                pan = clamp_int(pan, cfg.min_us, cfg.max_us)
                tilt = clamp_int(tilt, cfg.min_us, cfg.max_us)
                out.set_us(pan, tilt)
                if cfg.verbose:
                    sys.stderr.write(f"[set_pwm_client] SET_PWM -> PAN={pan} TILT={tilt}\n")
        except socket.timeout:
            continue


def main() -> int:
    p = argparse.ArgumentParser(description="Ubuntu 서버로부터 SET_PWM,PAN=...,TILT=...를 받아 GPIO12/13 PWM 출력")
    p.add_argument("--host", default="10.42.0.1", help="ubuntu_tcp_server가 떠 있는 호스트 IP")
    p.add_argument("--port", type=int, default=5555, help="ubuntu_tcp_server 포트")
    p.add_argument("--backend", choices=["pigpio", "sysfs"], default="pigpio", help="PWM 출력 방식")
    p.add_argument("--pigpio-host", default="localhost", help="pigpiod host")
    p.add_argument("--pigpio-port", type=int, default=8888, help="pigpiod port")
    p.add_argument("--period-ns", type=int, default=20_000_000, help="sysfs 백엔드에서 사용할 period(ns)")
    p.add_argument("--min-us", type=int, default=800, help="PWM 최소(us)")
    p.add_argument("--max-us", type=int, default=2200, help="PWM 최대(us)")
    p.add_argument("-v", "--verbose", action="store_true", help="로그 출력")
    args = p.parse_args()

    cfg = Config(
        host=str(args.host),
        port=int(args.port),
        backend=str(args.backend),
        pigpio_host=str(args.pigpio_host),
        pigpio_port=int(args.pigpio_port),
        period_ns=int(args.period_ns),
        min_us=int(args.min_us),
        max_us=int(args.max_us),
        verbose=bool(args.verbose),
    )

    out: PwmOut
    if cfg.backend == "pigpio":
        out = PigpioOut(cfg.pigpio_host, cfg.pigpio_port)
    else:
        out = SysfsOut(cfg.period_ns)

    out.start()
    try:
        connect_loop(cfg, out)
    except KeyboardInterrupt:
        return 0
    finally:
        out.stop()


if __name__ == "__main__":
    raise SystemExit(main())

