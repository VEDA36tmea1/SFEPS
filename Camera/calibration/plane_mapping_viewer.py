#!/usr/bin/env python3
"""
MATLAB fitgeotrans + transformPointsForward 와 1:1로 대응하는 평면 매핑 뷰어.

- 입력: pick_plane_points.py 로 만든 plane_points.json
  - pixel_points  -> movingPoints (이미지 픽셀 좌표)
  - world_points  -> fixedPoints (평면 실세계 좌표, 단위는 사용자가 정한 것)

- 동작:
  1) cv2.findHomography(movingPoints, fixedPoints, method=0) 로 H (pixel -> world) 계산
  2) H_inv = H^-1 (world -> pixel)
  3) 이미지 창에서 마우스로 픽셀을 클릭하면:
     - 해당 픽셀 (u,v)를 (X,Y) 평면 좌표로 변환해서 터미널/화면에 표시
  4) 필요하면 코드 맨 아래쪽에 예시처럼 임의의 (X,Y)를 다시 픽셀로 투영할 수도 있음.

조작:
- 마우스 왼쪽 클릭: 픽셀 좌표(u,v) 찍기 -> 터미널에 (u,v)와 대응하는 (X,Y) 출력
- q / ESC: 종료
"""

from __future__ import annotations

import json
import os
import sys
from typing import Any, Dict, List, Tuple

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
PLANE_JSON_PATH = os.path.join(HERE, "plane_points.json")


def load_plane_points(path: str) -> Tuple[str, List[Tuple[float, float]], List[Tuple[float, float]]]:
    with open(path, "r", encoding="utf-8") as f:
        data: Dict[str, Any] = json.load(f)

    img_path = data.get("image_path") or data.get("image")
    if img_path is None:
        raise KeyError("plane_points.json 에 image_path / image 가 없습니다.")

    if not os.path.isabs(img_path):
        img_path = os.path.join(HERE, img_path)

    pix = data.get("pixel_points", [])
    world = data.get("world_points", [])
    if len(pix) < 4 or len(world) < 4:
        raise ValueError("pixel_points / world_points 에 최소 4개 이상의 점이 필요합니다.")

    pixel_points: List[Tuple[float, float]] = [(float(p["x"]), float(p["y"])) for p in pix]
    world_points: List[Tuple[float, float]] = [(float(p["X"]), float(p["Y"])) for p in world]

    # findHomography는 최소 4점 이상이면 OK. 여기서는 앞의 4점을 기본으로 사용.
    # (원하면 더 많은 점을 넣어도 되지만, 지금은 4점 대응이 fitgeotrans 예제와 1:1)
    return img_path, pixel_points[:4], world_points[:4]


def compute_homography(
    pixel_points: List[Tuple[float, float]],
    world_points: List[Tuple[float, float]],
) -> Tuple[np.ndarray, np.ndarray]:
    """
    pixel_points -> world_points Homography (H) 와 역행렬(H_inv)을 계산.
    H: 3x3, pixel -> world  (u,v,1)^T -> (X,Y,1)^T (또는 스케일 후 정규화)
    """
    moving = np.array(pixel_points, dtype=np.float64)  # (N,2)
    fixed = np.array(world_points, dtype=np.float64)   # (N,2)

    # MATLAB: tform = fitgeotrans(moving, fixed, 'projective')
    # OpenCV: H 는 moving -> fixed projective 변환
    H, mask = cv2.findHomography(moving, fixed, method=0)
    if H is None:
        raise RuntimeError("findHomography 실패 (H=None)")
    try:
        H_inv = np.linalg.inv(H)
    except np.linalg.LinAlgError:
        raise RuntimeError("Homography 역행렬을 계산할 수 없습니다.")
    return H, H_inv


def apply_H(H: np.ndarray, u: float, v: float) -> Tuple[float, float]:
    """
    Homography H 를 (u,v) 픽셀 좌표에 적용하여 (X,Y) 평면 좌표를 반환.
    MATLAB: [xWorld, yWorld] = transformPointsForward(tform, u, v);
    """
    pt = np.array([u, v, 1.0], dtype=np.float64)
    w = H @ pt
    if abs(w[2]) < 1e-9:
        return float("nan"), float("nan")
    X = w[0] / w[2]
    Y = w[1] / w[2]
    return float(X), float(Y)


def apply_H_inv(H_inv: np.ndarray, X: float, Y: float) -> Tuple[float, float]:
    """
    Homography 역행렬(H_inv) 을 (X,Y) 평면 좌표에 적용해 (u,v) 픽셀 좌표를 반환.
    MATLAB: [u2, v2] = transformPointsInverse(tform, X, Y);
    """
    
    pt = np.array([X, Y, 1.0], dtype=np.float64)
    w = H_inv @ pt
    if abs(w[2]) < 1e-9:
        return float("nan"), float("nan")
    u = w[0] / w[2]
    v = w[1] / w[2]
    return float(u), float(v)


def main() -> None:
    json_arg = sys.argv[1] if len(sys.argv) >= 2 else PLANE_JSON_PATH
    if not os.path.isabs(json_arg):
        json_arg = os.path.join(HERE, json_arg)

    if not os.path.exists(json_arg):
        print(f"[ERROR] plane_points.json 없음: {json_arg}", file=sys.stderr)
        sys.exit(1)

    img_path, pixel_points, world_points = load_plane_points(json_arg)
    print(f"[INFO] image_path = {img_path}")
    print("[INFO] pixel_points (movingPoints):")
    for i, (u, v) in enumerate(pixel_points):
        print(f"  P{i+1} = ({u:.3f}, {v:.3f})")
    print("[INFO] world_points (fixedPoints):")
    for i, (X, Y) in enumerate(world_points):
        print(f"  W{i+1} = ({X:.3f}, {Y:.3f})")

    H, H_inv = compute_homography(pixel_points, world_points)
    print("\n[INFO] Homography H (pixel -> world):")
    print(H)

    img = cv2.imread(img_path)
    if img is None:
        print(f"[ERROR] imread 실패: {img_path}", file=sys.stderr)
        sys.exit(1)

    vis = img.copy()
    win = "plane mapping viewer (click: pixel->world, q/ESC: quit)"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)

    def redraw() -> None:
        nonlocal vis
        vis = img.copy()
        # P1~P4 표시
        for i, (u, v) in enumerate(pixel_points):
            cv2.circle(vis, (int(round(u)), int(round(v))), 6, (0, 255, 0), -1, cv2.LINE_AA)
            cv2.putText(
                vis,
                f"P{i+1}",
                (int(round(u)) + 8, int(round(v)) - 8),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.8,
                (0, 255, 0),
                2,
            )
        cv2.imshow(win, vis)

    def on_mouse(event: int, x: int, y: int, flags: int, param: Any) -> None:
        if event == cv2.EVENT_LBUTTONDOWN:
            X, Y = apply_H(H, float(x), float(y))
            print(f"[click] pixel=({x:.1f}, {y:.1f}) -> world=({X:.3f}, {Y:.3f})")
            cv2.circle(vis, (x, y), 5, (0, 0, 255), -1, cv2.LINE_AA)
            cv2.putText(
                vis,
                f"({X:.1f},{Y:.1f})",
                (x + 10, y - 10),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.6,
                (0, 0, 255),
                2,
            )
            cv2.imshow(win, vis)

    redraw()
    cv2.setMouseCallback(win, on_mouse)

    print("\n[INFO] 창에서 마우스로 픽셀을 클릭하면 world 좌표 (X,Y)를 터미널에 출력합니다.")
    print("[INFO] q 또는 ESC 로 종료.\n")

    while True:
        key = cv2.waitKey(20) & 0xFF
        if key in (27, ord("q")):
            break

    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()

