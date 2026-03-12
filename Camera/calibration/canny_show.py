#!/usr/bin/env python3
"""
Canny 임계값을 트랙바로 맞춘 뒤, 버튼(키) 한 번으로:
- HoughLinesP 로 직선 후보 추출
- 사용자가 마우스로 그은 기준선(최대 2개)과 유사한 라인만 필터링(각도/거리)
- 선택된 라인 주변 edge 포인트로 RANSAC 직선 피팅
- (기준선 2개가 있을 때) 두 직선 교점 표시

조작:
- **마우스 드래그**: 기준선 그리기 (최대 2개)
- **p**: 현재 Canny 결과로 전체 파이프라인 실행 (Hough→Filter→RANSAC→교점)
- **r**: 기준선/결과 리셋
- **q / ESC**: 종료

실행:
  python3 canny_show.py
  python3 canny_show.py 51.png
"""

from __future__ import annotations

import math
import os
import random
import sys
from dataclasses import dataclass
import json
from typing import List, Optional, Tuple

import cv2
import numpy as np


@dataclass
class LineSeg:
    p0: Tuple[int, int]
    p1: Tuple[int, int]

    def direction(self) -> np.ndarray:
        v = np.array([self.p1[0] - self.p0[0], self.p1[1] - self.p0[1]], dtype=np.float64)
        n = np.linalg.norm(v)
        if n < 1e-9:
            return np.array([1.0, 0.0], dtype=np.float64)
        return v / n

    def angle_deg(self) -> float:
        v = self.direction()
        ang = math.degrees(math.atan2(v[1], v[0]))
        # [-90, 90) 로 정규화 (기울기만 비교하기 위함)
        while ang >= 90.0:
            ang -= 180.0
        while ang < -90.0:
            ang += 180.0
        return ang

    def length_px(self) -> float:
        return float(math.hypot(self.p1[0] - self.p0[0], self.p1[1] - self.p0[1]))


def angle_diff_deg(a: float, b: float) -> float:
    d = abs(a - b)
    return min(d, 180.0 - d)


def point_line_distance(px: float, py: float, p0: Tuple[int, int], p1: Tuple[int, int]) -> float:
    x0, y0 = p0
    x1, y1 = p1
    vx = x1 - x0
    vy = y1 - y0
    denom = math.hypot(vx, vy)
    if denom < 1e-9:
        return math.hypot(px - x0, py - y0)
    # 2D line distance using cross product magnitude / |v|
    return abs(vy * px - vx * py + x1 * y0 - y1 * x0) / denom


def hough_lines_p(edges: np.ndarray) -> List[LineSeg]:
    lines = cv2.HoughLinesP(
        edges,
        rho=1,
        theta=np.pi / 180.0,
        threshold=80,
        minLineLength=120,
        maxLineGap=25,
    )
    out: List[LineSeg] = []
    if lines is None:
        return out
    for l in lines.reshape(-1, 4):
        x1, y1, x2, y2 = map(int, l.tolist())
        out.append(LineSeg((x1, y1), (x2, y2)))
    return out


def filter_by_reference(
    candidates: List[LineSeg],
    ref: LineSeg,
    angle_tol_deg: float = 7.0,
    dist_tol_px: float = 40.0,
) -> List[LineSeg]:
    ref_ang = ref.angle_deg()
    out: List[LineSeg] = []
    for ln in candidates:
        if angle_diff_deg(ln.angle_deg(), ref_ang) > angle_tol_deg:
            continue
        mx = 0.5 * (ln.p0[0] + ln.p1[0])
        my = 0.5 * (ln.p0[1] + ln.p1[1])
        if point_line_distance(mx, my, ref.p0, ref.p1) > dist_tol_px:
            continue
        out.append(ln)
    return out


def collect_edge_points_near_lines(edges: np.ndarray, lines: List[LineSeg], band_px: int = 3) -> np.ndarray:
    ys, xs = np.where(edges > 0)
    if len(xs) == 0:
        return np.zeros((0, 2), dtype=np.float64)
    pts = np.stack([xs.astype(np.float64), ys.astype(np.float64)], axis=1)
    if not lines:
        return np.zeros((0, 2), dtype=np.float64)

    keep = np.zeros((pts.shape[0],), dtype=bool)
    for ln in lines:
        # 빠르게: 샘플링해서 마스크 만들기 (전체 pts에 대해 거리 계산)
        # pts 수가 많으면 느릴 수 있지만, 단일 이미지 디버깅 용도라 OK.
        x0, y0 = ln.p0
        x1, y1 = ln.p1
        vx = x1 - x0
        vy = y1 - y0
        denom = math.hypot(vx, vy)
        if denom < 1e-9:
            continue
        # 거리: |vy*x - vx*y + c| / denom
        c = x1 * y0 - y1 * x0
        d = np.abs(vy * pts[:, 0] - vx * pts[:, 1] + c) / denom
        keep |= (d <= float(band_px))
    return pts[keep]


def ransac_fit_line(points: np.ndarray, iters: int = 400, inlier_thresh_px: float = 2.0) -> Optional[Tuple[float, float, float]]:
    """
    ax + by + c = 0 형태로 반환 (a^2+b^2=1 정규화).
    """
    n = points.shape[0]
    if n < 2:
        return None

    best_inliers = -1
    best_model: Optional[Tuple[float, float, float]] = None

    idxs = list(range(n))
    for _ in range(iters):
        i, j = random.sample(idxs, 2)
        x1, y1 = points[i]
        x2, y2 = points[j]
        if abs(x1 - x2) < 1e-6 and abs(y1 - y2) < 1e-6:
            continue

        # 두 점으로 직선: (y1-y2)x + (x2-x1)y + (x1*y2 - x2*y1)=0
        a = y1 - y2
        b = x2 - x1
        c = x1 * y2 - x2 * y1
        norm = math.hypot(a, b)
        if norm < 1e-9:
            continue
        a /= norm
        b /= norm
        c /= norm

        d = np.abs(a * points[:, 0] + b * points[:, 1] + c)
        inliers = int(np.sum(d <= inlier_thresh_px))
        if inliers > best_inliers:
            best_inliers = inliers
            best_model = (a, b, c)

    if best_model is None or best_inliers < 20:
        return None

    # inlier로 재추정 (SVD)
    a0, b0, c0 = best_model
    d = np.abs(a0 * points[:, 0] + b0 * points[:, 1] + c0)
    inlier_pts = points[d <= inlier_thresh_px]
    if inlier_pts.shape[0] < 2:
        return None

    # 점들의 중심을 빼고, 최소자승으로 직선의 법선 구하기
    mean = np.mean(inlier_pts, axis=0)
    X = inlier_pts - mean
    _, _, vt = np.linalg.svd(X, full_matrices=False)
    direction = vt[0]  # 가장 큰 분산 방향
    # 법선 = direction 을 90도 회전
    nx, ny = -direction[1], direction[0]
    norm = math.hypot(nx, ny)
    if norm < 1e-9:
        return None
    nx /= norm
    ny /= norm
    c = -(nx * mean[0] + ny * mean[1])
    return (nx, ny, c)


def intersect_lines(l1: Tuple[float, float, float], l2: Tuple[float, float, float]) -> Optional[Tuple[float, float]]:
    a1, b1, c1 = l1
    a2, b2, c2 = l2
    det = a1 * b2 - a2 * b1
    if abs(det) < 1e-9:
        return None
    x = (b1 * c2 - b2 * c1) / det
    y = (c1 * a2 - c2 * a1) / det
    return (x, y)


def draw_infinite_line(img: np.ndarray, model: Tuple[float, float, float], color: Tuple[int, int, int], thickness: int = 2) -> None:
    a, b, c = model
    h, w = img.shape[:2]
    # x=0, x=w-1 에서 y 계산 (b!=0), 아니면 y=0, y=h-1 에서 x 계산
    pts: List[Tuple[int, int]] = []
    if abs(b) > 1e-6:
        y0 = int(round((-c - a * 0.0) / b))
        y1 = int(round((-c - a * float(w - 1)) / b))
        pts = [(0, y0), (w - 1, y1)]
    elif abs(a) > 1e-6:
        x0 = int(round((-c - b * 0.0) / a))
        x1 = int(round((-c - b * float(h - 1)) / a))
        pts = [(x0, 0), (x1, h - 1)]
    if len(pts) == 2:
        cv2.line(img, pts[0], pts[1], color, thickness, cv2.LINE_AA)


def main() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    img_name = sys.argv[1] if len(sys.argv) >= 2 else "51.png"
    img_path = img_name if os.path.isabs(img_name) else os.path.join(here, img_name)

    max_refs = 5
    img0 = cv2.imread(img_path)
    if img0 is None:
        print(f"[ERROR] imread 실패: {img_path}", file=sys.stderr)
        sys.exit(1)

    gray0 = cv2.cvtColor(img0, cv2.COLOR_BGR2GRAY)
    gray0 = cv2.GaussianBlur(gray0, (5, 5), 1.2)

    out_json_path = os.path.join(here, "ref_lines.json")
    win = "canny+hough+ransac (drag=ref line, p=process, s=save, r=reset, q/ESC=quit)"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)

    def on_change(_: int) -> None:
        pass

    cv2.createTrackbar("low", win, 50, 500, on_change)
    cv2.createTrackbar("high", win, 150, 500, on_change)

    refs: List[LineSeg] = []
    dragging = False
    drag_p0: Tuple[int, int] = (0, 0)
    drag_p1: Tuple[int, int] = (0, 0)

    last_candidates: List[LineSeg] = []
    last_selected1: List[LineSeg] = []
    last_selected2: List[LineSeg] = []
    last_fit1: Optional[Tuple[float, float, float]] = None
    last_fit2: Optional[Tuple[float, float, float]] = None
    last_inter: Optional[Tuple[float, float]] = None

    def on_mouse(event: int, x: int, y: int, flags: int, param) -> None:
        nonlocal dragging, drag_p0, drag_p1, refs
        if event == cv2.EVENT_LBUTTONDOWN:
            dragging = True
            drag_p0 = (x, y)
            drag_p1 = (x, y)
        elif event == cv2.EVENT_MOUSEMOVE and dragging:
            drag_p1 = (x, y)
        elif event == cv2.EVENT_LBUTTONUP and dragging:
            dragging = False
            drag_p1 = (x, y)
            if len(refs) < max_refs:
                refs.append(LineSeg(drag_p0, drag_p1))

    cv2.setMouseCallback(win, on_mouse)

    while True:
        low = cv2.getTrackbarPos("low", win)
        high = cv2.getTrackbarPos("high", win)
        if high < low:
            high = low
            cv2.setTrackbarPos("high", win, high)

        edges = cv2.Canny(gray0, low, high, L2gradient=True)
        vis = cv2.cvtColor(edges, cv2.COLOR_GRAY2BGR)

        # Hough 후보(연한 파랑)
        for ln in last_candidates:
            cv2.line(vis, ln.p0, ln.p1, (255, 200, 80), 1, cv2.LINE_AA)

        # ref 선(노랑/주황)
        for i, r in enumerate(refs):
            col = (0, 255, 255) if i == 0 else (0, 180, 255)
            cv2.line(vis, r.p0, r.p1, col, 2, cv2.LINE_AA)

        # 드래그 중 임시 ref
        if dragging:
            cv2.line(vis, drag_p0, drag_p1, (0, 255, 255), 1, cv2.LINE_AA)

        # 선택된 Hough 라인(초록/연두)
        for ln in last_selected1:
            cv2.line(vis, ln.p0, ln.p1, (0, 255, 0), 2, cv2.LINE_AA)
        for ln in last_selected2:
            cv2.line(vis, ln.p0, ln.p1, (0, 200, 0), 2, cv2.LINE_AA)

        # RANSAC 피팅 결과(빨강/자홍)
        if last_fit1 is not None:
            draw_infinite_line(vis, last_fit1, (0, 0, 255), 2)
        if last_fit2 is not None:
            draw_infinite_line(vis, last_fit2, (255, 0, 255), 2)

        # 교점(하양)
        if last_inter is not None:
            ix, iy = last_inter
            cv2.circle(vis, (int(round(ix)), int(round(iy))), 7, (255, 255, 255), -1, cv2.LINE_AA)

        cv2.putText(
            vis,
            f"{os.path.basename(img_path)} | low={low} high={high} | refs={len(refs)} | p=process s=save r=reset",
            (10, 30),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.75,
            (0, 255, 0),
            2,
        )
        cv2.imshow(win, vis)

        key = cv2.waitKey(10) & 0xFF
        if key in (27, ord("q")):
            break
        if key == ord("r"):
            refs = []
            last_candidates = []
            last_selected1 = []
            last_selected2 = []
            last_fit1 = None
            last_fit2 = None
            last_inter = None
        if key == ord("p"):
            # 1) Hough 후보
            last_candidates = hough_lines_p(edges)

            # 2) ref 기반 필터링
            last_selected1 = filter_by_reference(last_candidates, refs[0]) if len(refs) >= 1 else []
            last_selected2 = filter_by_reference(last_candidates, refs[1]) if len(refs) >= 2 else []

            # 3) RANSAC 피팅
            pts1 = collect_edge_points_near_lines(edges, last_selected1, band_px=3)
            pts2 = collect_edge_points_near_lines(edges, last_selected2, band_px=3)
            last_fit1 = ransac_fit_line(pts1, iters=500, inlier_thresh_px=2.0) if pts1.shape[0] else None
            last_fit2 = ransac_fit_line(pts2, iters=500, inlier_thresh_px=2.0) if pts2.shape[0] else None

            # 4) 교점
            last_inter = None
            if last_fit1 is not None and last_fit2 is not None:
                last_inter = intersect_lines(last_fit1, last_fit2)
        if key == ord("s"):
            payload = {
                "image": os.path.basename(img_path),
                "image_path": img_path,
                "canny": {"low": int(low), "high": int(high)},
                "refs": [
                    {
                        "p0": [int(r.p0[0]), int(r.p0[1])],
                        "p1": [int(r.p1[0]), int(r.p1[1])],
                        "angle_deg": float(r.angle_deg()),
                        "length_px": float(r.length_px()),
                    }
                    for r in refs
                ],
            }
            if last_inter is not None:
                payload["intersection_px"] = [float(last_inter[0]), float(last_inter[1])]

            print("\n[save] reference lines")
            print(json.dumps(payload, indent=2, ensure_ascii=False))
            try:
                with open(out_json_path, "w", encoding="utf-8") as f:
                    json.dump(payload, f, indent=2, ensure_ascii=False)
                print(f"[save] wrote: {out_json_path}")
            except OSError as e:
                print(f"[save][ERROR] failed to write {out_json_path}: {e}", file=sys.stderr)

    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()

