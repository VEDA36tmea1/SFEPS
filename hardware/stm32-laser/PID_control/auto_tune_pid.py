#!/usr/bin/env python3
"""
STM32 PID log -> simple sysid -> PID auto search

Expected log format (from STM32 USART2):
PIDLOG,t:12345,ex:-10.00,ey:5.00,out_x:1432,out_y:1251,kpx:0.0100,kix:0.0000,kdx:0.0000,kpy:-0.0100,kiy:0.0000,kdy:0.0000
"""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
from scipy.optimize import minimize
import control as ctrl


LOG_RE = re.compile(
    r"PIDLOG,t:(?P<t>\d+),ex:(?P<ex>-?\d+\.?\d*),ey:(?P<ey>-?\d+\.?\d*),"
    r"out_x:(?P<out_x>\d+),out_y:(?P<out_y>\d+),"
    r"kpx:(?P<kpx>-?\d+\.?\d*),kix:(?P<kix>-?\d+\.?\d*),kdx:(?P<kdx>-?\d+\.?\d*),"
    r"kpy:(?P<kpy>-?\d+\.?\d*),kiy:(?P<kiy>-?\d+\.?\d*),kdy:(?P<kdy>-?\d+\.?\d*)"
)


@dataclass
class AxisTuneResult:
    kp: float
    ki: float
    kd: float
    k_plant: float
    tau: float


def parse_log(log_path: Path) -> pd.DataFrame:
    rows = []
    for line in log_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        m = LOG_RE.search(line.strip())
        if not m:
            continue
        rows.append({k: float(v) for k, v in m.groupdict().items()})
    if not rows:
        raise ValueError("PIDLOG 라인을 찾지 못했습니다. 로그 포맷을 확인하세요.")
    df = pd.DataFrame(rows)
    df = df.sort_values("t").reset_index(drop=True)
    return df


def estimate_first_order(ts: float, e: np.ndarray, u: np.ndarray) -> tuple[float, float]:
    """
    ARX approximation:
      e[k+1] = a*e[k] + b*u[k]
    to first-order continuous plant:
      G(s) = K / (tau*s + 1)
    """
    if len(e) < 5:
        raise ValueError("데이터가 너무 적습니다. 최소 5개 이상의 샘플이 필요합니다.")

    y = e[1:]
    x1 = e[:-1]
    x2 = u[:-1]
    X = np.column_stack([x1, x2])
    coeff, *_ = np.linalg.lstsq(X, y, rcond=None)
    a, b = coeff[0], coeff[1]

    a = float(np.clip(a, 1e-5, 0.9999))
    tau = -ts / np.log(a)
    k = b / max(1.0 - a, 1e-6)
    return float(k), float(max(tau, 1e-3))


def make_pid_tf(kp: float, ki: float, kd: float, ts: float):
    z = ctrl.TransferFunction.z
    p_term = kp
    i_term = ki * ts / (z - 1)
    d_term = kd * (z - 1) / ts
    return p_term + i_term + d_term


def pid_cost(params: np.ndarray, plant, ts: float) -> float:
    kp, ki, kd = params
    if kp < 0 or ki < 0 or kd < 0:
        return 1e9
    c = make_pid_tf(kp, ki, kd, ts)
    closed = ctrl.feedback(c * plant, 1)

    t = np.arange(0.0, 2.0, ts)
    t, y = ctrl.step_response(closed, T=t)
    y = np.asarray(y).flatten()
    e = 1.0 - y

    overshoot = max(0.0, np.max(y) - 1.0)
    iae = np.trapz(np.abs(e), t)
    final_err = abs(e[-1])
    return 5.0 * overshoot + iae + 2.0 * final_err


def tune_axis(ts: float, e_axis: np.ndarray, u_axis: np.ndarray, init_kp: float) -> AxisTuneResult:
    k_plant, tau = estimate_first_order(ts, e_axis, u_axis)
    plant = ctrl.TransferFunction([k_plant], [tau, 1.0])
    plant = ctrl.sample_system(plant, Ts=ts, method="zoh")

    x0 = np.array([max(init_kp, 1e-4), 1e-4, 1e-4], dtype=float)
    bounds = [(1e-5, 2.0), (0.0, 2.0), (0.0, 0.2)]
    res = minimize(pid_cost, x0=x0, args=(plant, ts), method="L-BFGS-B", bounds=bounds)

    kp, ki, kd = res.x
    return AxisTuneResult(float(kp), float(ki), float(kd), k_plant, tau)


def main():
    parser = argparse.ArgumentParser(description="PIDLOG 기반 자동 PID 튜닝")
    parser.add_argument("--log", required=True, help="STM32 PID 로그 txt 파일 경로")
    parser.add_argument("--plot", action="store_true", help="단위 계단 응답 그래프 표시")
    args = parser.parse_args()

    df = parse_log(Path(args.log))
    t_ms = df["t"].to_numpy()
    ts = float(np.median(np.diff(t_ms)) / 1000.0)
    if ts <= 0:
        raise ValueError("샘플링 시간 추정 실패")

    # X축 튜닝
    ex = df["ex"].to_numpy()
    ux = df["out_x"].to_numpy()
    init_kpx = abs(float(df["kpx"].iloc[-1])) if "kpx" in df else 0.01
    rx = tune_axis(ts, ex, ux, init_kpx)

    # Y축 튜닝 (부호는 기존 방향 유지: kpy 음수)
    ey = df["ey"].to_numpy()
    uy = df["out_y"].to_numpy()
    init_kpy = abs(float(df["kpy"].iloc[-1])) if "kpy" in df else 0.01
    ry = tune_axis(ts, ey, uy, init_kpy)

    print("=== Auto Tune Result (apply with sign convention) ===")
    print(f"[X axis / PA0] kp={rx.kp:.6f}, ki={rx.ki:.6f}, kd={rx.kd:.6f}")
    print(f"  estimated plant: K={rx.k_plant:.6f}, tau={rx.tau:.6f}s")
    print(f"[Y axis / PA8] kp={-ry.kp:.6f}, ki={-ry.ki:.6f}, kd={-ry.kd:.6f}")
    print(f"  estimated plant: K={ry.k_plant:.6f}, tau={ry.tau:.6f}s")
    print("")
    print("STM32 적용 예시:")
    print(
        f'IbvsPid_AxisInit(&pid_x, {rx.kp:.6f}f, {rx.ki:.6f}f, {rx.kd:.6f}f, (float)ux_init);'
    )
    print(
        f'IbvsPid_AxisInit(&pid_y, {-ry.kp:.6f}f, {-ry.ki:.6f}f, {-ry.kd:.6f}f, (float)uy_init);'
    )

    if args.plot:
        # quick visualization for X axis
        plant_x = ctrl.TransferFunction([rx.k_plant], [rx.tau, 1.0])
        plant_xd = ctrl.sample_system(plant_x, Ts=ts, method="zoh")
        c_x = make_pid_tf(rx.kp, rx.ki, rx.kd, ts)
        closed_x = ctrl.feedback(c_x * plant_xd, 1)
        t = np.arange(0.0, 2.0, ts)
        t, y = ctrl.step_response(closed_x, T=t)
        plt.figure(figsize=(7, 4))
        plt.plot(t, y, label="Closed-loop step (X)")
        plt.axhline(1.0, color="k", linestyle="--", linewidth=1)
        plt.xlabel("Time [s]")
        plt.ylabel("Normalized output")
        plt.title("Auto-tuned PID step response (X axis)")
        plt.grid(True, alpha=0.3)
        plt.legend()
        plt.tight_layout()
        plt.show()


if __name__ == "__main__":
    main()
