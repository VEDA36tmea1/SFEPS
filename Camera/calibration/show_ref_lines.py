#!/usr/bin/env python3
"""
ref_lines.json(기준선 저장 파일)을 읽어서, 원본 종횡비를 유지한 상태로 화면에 표시.

- JSON 안의 p0/p1 좌표는 "원본 이미지 픽셀 좌표"라고 가정.
- 표시용으로 창 크기에 맞춰 리사이즈(비율 유지)하며, 선/교점도 같은 비율로 스케일링해서 그린다.

사용:
  python3 show_ref_lines.py
  python3 show_ref_lines.py ref_lines.json
  python3 show_ref_lines.py ref_lines.json 67.png

키:
- **SPACE**: "원본 위에 왜곡 반영(휘어진 polyline)" 토글
- **q / ESC**: 종료
"""

from __future__ import annotations

import json
import os
import sys
from typing import Any, Dict, List, Tuple

import cv2
import numpy as np

try:
    from scipy.io import loadmat
except Exception:  # pragma: no cover
    loadmat = None


def load_json(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def fit_size_keep_aspect(w: int, h: int, max_w: int, max_h: int) -> Tuple[int, int, float]:
    """원본(w,h)을 최대(max_w,max_h) 안에 비율 유지로 맞추고 scale 반환."""
    if w <= 0 or h <= 0:
        return w, h, 1.0
    s = min(max_w / float(w), max_h / float(h))
    s = max(s, 1e-9)
    return int(round(w * s)), int(round(h * s)), float(s)


def load_intrinsics_from_opencv_mat(mat_path: str) -> Tuple[np.ndarray, np.ndarray]:
    if loadmat is None:
        raise RuntimeError("scipy가 필요합니다. (venv에서) `pip install scipy` 후 다시 실행하세요.")
    data = loadmat(mat_path, squeeze_me=True, struct_as_record=False)
    if "cameraMatrix" not in data or "distCoeffs" not in data:
        raise KeyError(f"{mat_path} 에 cameraMatrix/distCoeffs 키가 없습니다.")
    K = np.array(data["cameraMatrix"], dtype=np.float64)
    dist = np.array(data["distCoeffs"], dtype=np.float64).reshape(-1)
    return K, dist


def distort_normalized_points(xy: np.ndarray, dist: np.ndarray) -> np.ndarray:
    """
    normalized plane (x,y, z=1)에서 distortion 적용.
    dist: (k1,k2,p1,p2,k3[,k4,k5,k6])
    반환: distorted normalized (x_d, y_d)
    """
    x = xy[:, 0]
    y = xy[:, 1]
    r2 = x * x + y * y

    k1 = float(dist[0]) if dist.size > 0 else 0.0
    k2 = float(dist[1]) if dist.size > 1 else 0.0
    p1 = float(dist[2]) if dist.size > 2 else 0.0
    p2 = float(dist[3]) if dist.size > 3 else 0.0
    k3 = float(dist[4]) if dist.size > 4 else 0.0

    radial = 1.0 + k1 * r2 + k2 * (r2**2) + k3 * (r2**3)
    x_rad = x * radial
    y_rad = y * radial

    x_tan = 2.0 * p1 * x * y + p2 * (r2 + 2.0 * x * x)
    y_tan = p1 * (r2 + 2.0 * y * y) + 2.0 * p2 * x * y

    xd = x_rad + x_tan
    yd = y_rad + y_tan
    return np.stack([xd, yd], axis=1)


def normalized_to_pixel(xy: np.ndarray, K: np.ndarray) -> np.ndarray:
    fx = float(K[0, 0])
    fy = float(K[1, 1])
    cx = float(K[0, 2])
    cy = float(K[1, 2])
    u = fx * xy[:, 0] + cx
    v = fy * xy[:, 1] + cy
    return np.stack([u, v], axis=1)


def make_distorted_polyline_from_endpoints(
    p0_px: Tuple[float, float],
    p1_px: Tuple[float, float],
    K: np.ndarray,
    dist: np.ndarray,
    samples: int = 80,
) -> np.ndarray:
    """
    원본 픽셀 endpoints -> undistort -> 그 사이를 직선으로 샘플링 -> distortion 적용 -> 원본 픽셀 polyline 반환
    """
    pts = np.array([[p0_px], [p1_px]], dtype=np.float64)  # (2,1,2)
    und = cv2.undistortPoints(pts, K, dist)  # (2,1,2) normalized
    u0 = und[0, 0, :]
    u1 = und[1, 0, :]

    t = np.linspace(0.0, 1.0, int(samples), dtype=np.float64)[:, None]
    line_und = (1.0 - t) * u0[None, :] + t * u1[None, :]

    line_dist_norm = distort_normalized_points(line_und, dist)
    line_dist_px = normalized_to_pixel(line_dist_norm, K)
    return line_dist_px  # (N,2)


def main() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    json_path = sys.argv[1] if len(sys.argv) >= 2 else os.path.join(here, "ref_lines.json")
    img_arg = sys.argv[2] if len(sys.argv) >= 3 else None

    if not os.path.isabs(json_path):
        json_path = os.path.join(here, json_path)
    if not os.path.exists(json_path):
        print(f"[ERROR] json 없음: {json_path}", file=sys.stderr)
        sys.exit(1)

    data = load_json(json_path)

    # 이미지 경로 결정: CLI 우선, 그 다음 json의 image_path, 마지막으로 json의 image(base name)
    img_path = None
    if img_arg:
        img_path = img_arg if os.path.isabs(img_arg) else os.path.join(here, img_arg)
    elif "image_path" in data and data["image_path"]:
        img_path = str(data["image_path"])
        if not os.path.isabs(img_path):
            img_path = os.path.join(here, img_path)
    elif "image" in data and data["image"]:
        img_path = os.path.join(here, str(data["image"]))

    if not img_path or not os.path.exists(img_path):
        print(f"[ERROR] 이미지 경로를 찾을 수 없습니다. json의 image_path/image를 확인하세요. img_path={img_path}", file=sys.stderr)
        sys.exit(1)

    img0 = cv2.imread(img_path)
    if img0 is None:
        print(f"[ERROR] imread 실패: {img_path}", file=sys.stderr)
        sys.exit(1)

    h0, w0 = img0.shape[:2]

    # intrinsics 로드 (왜곡 토글을 위해)
    mat_path = os.path.join(here, "cameraParams_opencv.mat")
    K = None
    dist = None
    if os.path.exists(mat_path):
        try:
            K, dist = load_intrinsics_from_opencv_mat(mat_path)
        except Exception as e:
            print(f"[WARN] intrinsics 로드 실패: {e}", file=sys.stderr)

    # 표시 크기(모니터에 맞추기). 너무 크면 1280x720 안에 맞춤.
    disp_w, disp_h, s = fit_size_keep_aspect(w0, h0, max_w=1280, max_h=720)

    def sx(x: float) -> int:
        return int(round(float(x) * s))

    def sy(y: float) -> int:
        return int(round(float(y) * s))

    refs = data.get("refs", [])
    colors = [
        (0, 255, 255),
        (0, 180, 255),
        (255, 0, 0),
        (255, 0, 255),
        (0, 255, 0),
        (0, 128, 255),
    ]

    inter = data.get("intersection_px", None)
    canny = data.get("canny", {})
    low = canny.get("low", "?")
    high = canny.get("high", "?")

    def render(distort_draw: bool) -> np.ndarray:
        img = cv2.resize(
            img0, (disp_w, disp_h), interpolation=cv2.INTER_AREA if s < 1.0 else cv2.INTER_LINEAR
        ).copy()

        # 기준선 그리기
        for i, r in enumerate(refs):
            p0 = r.get("p0", None)
            p1 = r.get("p1", None)
            if not p0 or not p1 or len(p0) != 2 or len(p1) != 2:
                continue
            x0, y0 = float(p0[0]), float(p0[1])
            x1, y1 = float(p1[0]), float(p1[1])
            col = colors[i % len(colors)]

            if distort_draw and (K is not None) and (dist is not None):
                poly = make_distorted_polyline_from_endpoints((x0, y0), (x1, y1), K, dist, samples=90)
                pts = np.stack([np.round(poly[:, 0] * s), np.round(poly[:, 1] * s)], axis=1).astype(np.int32)
                cv2.polylines(img, [pts.reshape(-1, 1, 2)], False, col, 2, cv2.LINE_AA)
            else:
                cv2.line(img, (sx(x0), sy(y0)), (sx(x1), sy(y1)), col, 2, cv2.LINE_AA)

            cv2.putText(
                img,
                f"ref{i+1}",
                (sx(x0) + 5, sy(y0) - 5),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.7,
                col,
                2,
            )

        # 교점(있으면) 그리기 (그 자체는 점이라 왜곡 적용 의미가 애매해서 그대로 표시)
        if inter and isinstance(inter, (list, tuple)) and len(inter) == 2:
            ix, iy = float(inter[0]), float(inter[1])
            cv2.circle(img, (sx(ix), sy(iy)), 7, (255, 255, 255), -1, cv2.LINE_AA)
            cv2.putText(
                img,
                "P1",
                (sx(ix) + 10, sy(iy) - 10),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.8,
                (255, 255, 255),
                2,
            )

        mode = "distort_draw=ON" if distort_draw else "distort_draw=OFF"
        intr_ok = "K/dist OK" if (K is not None and dist is not None) else "K/dist missing"
        cv2.putText(
            img,
            f"{os.path.basename(img_path)} | scale={s:.3f} | refs={len(refs)} | canny=({low},{high}) | {mode} | {intr_ok}",
            (10, 30),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.75,
            (0, 255, 0),
            2,
        )
        cv2.putText(
            img,
            "SPACE: toggle distort-draw | q/ESC: quit",
            (10, 60),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.7,
            (0, 255, 0),
            2,
        )
        return img

    win = "ref_lines viewer"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)
    distort_draw = False
    cv2.imshow(win, render(distort_draw))
    while True:
        key = cv2.waitKey(0) & 0xFF
        if key == ord(" "):
            distort_draw = not distort_draw
            cv2.imshow(win, render(distort_draw))
            continue
        if key in (27, ord("q")):
            break
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()

