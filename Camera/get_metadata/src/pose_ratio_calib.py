#!/usr/bin/env python3
"""
이미지 1장으로 '1200mm 지점에 해당하는 bbox 비율(ratio)'을 캘리브레이션하는 도구.

사용:
  python3 pose_ratio_calib.py /path/to/image.png [--out ratio.json]

동작:
  - MediaPipe Pose(33 landmarks) 실행
  - pose landmarks로 사람 bbox(top/bottom/height)를 계산(visibility 임계값 적용)
  - 어깨(11/12) 및 head-anchors(코/눈/귀 등)를 표시
  - 마우스 좌클릭: "이 픽셀이 1200mm 지점"이라고 가정하고 ratio 계산:
      ratio = (click_y - bbox_top) / bbox_height
    그리고 target_y = bbox_top + bbox_height * ratio 를 화면에 가이드 라인으로 표시
  - 's' 키: ratio를 JSON으로 저장
  - 'r' 키: 클릭 초기화
  - 'q' 또는 ESC: 종료

주의:
  - 여기서 bbox는 ONVIF bbox가 아니라, MediaPipe landmark들로 만든 bbox입니다.
  - ONVIF bbox 기반 ratio를 얻고 싶으면, camera_client에서 '선택된 ONVIF bbox'를 사용하도록 확장하면 됩니다.
"""

from __future__ import annotations

import argparse
import json
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Tuple

import cv2
import mediapipe as mp
import numpy as np


POSE_LM = {
    "NOSE": 0,
    "L_SHOULDER": 11,
    "R_SHOULDER": 12,
    "L_HIP": 23,
    "R_HIP": 24,
    "L_EAR": 7,
    "R_EAR": 8,
    "L_EYE": 2,
    "R_EYE": 5,
}


@dataclass
class State:
    bbox: Optional[Tuple[int, int, int, int]] = None  # x0,y0,x1,y1
    ratio: Optional[float] = None
    click_xy: Optional[Tuple[int, int]] = None
    img_path: str = ""


def lm_to_px(lm, w: int, h: int) -> Tuple[int, int]:
    return int(lm.x * w), int(lm.y * h)


def compute_bbox(lms, w: int, h: int, vis_th: float = 0.5) -> Optional[Tuple[int, int, int, int]]:
    xs = []
    ys = []
    for lm in lms:
        if getattr(lm, "visibility", 1.0) < vis_th:
            continue
        xs.append(lm.x)
        ys.append(lm.y)
    if not xs or not ys:
        return None
    x0 = max(0, min(w - 1, int(min(xs) * w)))
    x1 = max(0, min(w - 1, int(max(xs) * w)))
    y0 = max(0, min(h - 1, int(min(ys) * h)))
    y1 = max(0, min(h - 1, int(max(ys) * h)))
    if x1 <= x0 or y1 <= y0:
        return None
    return x0, y0, x1, y1


def draw_cross(img, x: int, y: int, color, size: int = 8, thick: int = 2):
    cv2.line(img, (x - size, y), (x + size, y), color, thick, cv2.LINE_AA)
    cv2.line(img, (x, y - size), (x, y + size), color, thick, cv2.LINE_AA)


def on_mouse(event, x, y, flags, userdata):
    st: State = userdata
    if event != cv2.EVENT_LBUTTONDOWN:
        return
    if not st.bbox:
        return
    x0, y0, x1, y1 = st.bbox
    bh = max(1, y1 - y0)
    st.click_xy = (x, y)
    st.ratio = float((y - y0) / bh)
    print(f"[ratio] click=({x},{y}) bbox_top={y0} bbox_h={bh} ratio={st.ratio:.6f}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", help="입력 이미지 경로(png/jpg)")
    ap.add_argument("--out", default="", help="저장할 JSON 경로(옵션)")
    args = ap.parse_args()

    img_path = str(Path(args.image).expanduser().resolve())
    img = cv2.imread(img_path)
    if img is None or img.size == 0:
        print(f"[err] failed to read image: {img_path}")
        return 2

    h, w = img.shape[:2]
    mp_pose = mp.solutions.pose
    pose = mp_pose.Pose(
        static_image_mode=True,
        model_complexity=1,
        enable_segmentation=False,
        min_detection_confidence=0.5,
        min_tracking_confidence=0.5,
    )

    rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
    t0 = time.time()
    res = pose.process(rgb)
    dt_ms = (time.time() - t0) * 1000.0
    print(f"[pose] process_ms={dt_ms:.1f}")

    if not res.pose_landmarks:
        print("[pose] not found")
        return 0

    lms = res.pose_landmarks.landmark
    bbox = compute_bbox(lms, w, h, vis_th=0.4)
    st = State(bbox=bbox, ratio=None, click_xy=None, img_path=img_path)

    win = "pose_ratio_calib"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)
    cv2.setMouseCallback(win, on_mouse, st)

    while True:
        vis = img.copy()

        if st.bbox:
            x0, y0, x1, y1 = st.bbox
            cv2.rectangle(vis, (x0, y0), (x1, y1), (0, 255, 255), 2)
            cv2.putText(vis, f"bbox h={y1-y0}", (x0, max(0, y0 - 8)),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 255), 2)

        # shoulders
        for idx, name, color in [
            (POSE_LM["L_SHOULDER"], "L_SH", (0, 255, 0)),
            (POSE_LM["R_SHOULDER"], "R_SH", (0, 0, 255)),
        ]:
            lm = lms[idx]
            x, y = lm_to_px(lm, w, h)
            cv2.circle(vis, (x, y), 8, color, -1, cv2.LINE_AA)
            cv2.putText(vis, name, (x + 8, y - 8),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.6, color, 2)

        # head anchors (rough top)
        head_ids = [
            POSE_LM["NOSE"],
            POSE_LM["L_EAR"], POSE_LM["R_EAR"],
            POSE_LM["L_EYE"], POSE_LM["R_EYE"],
        ]
        head_pts = []
        for idx in head_ids:
            lm = lms[idx]
            if getattr(lm, "visibility", 1.0) < 0.4:
                continue
            head_pts.append(lm_to_px(lm, w, h))
        if head_pts:
            for (x, y) in head_pts:
                cv2.circle(vis, (x, y), 4, (255, 0, 0), -1, cv2.LINE_AA)
            top_y = min(y for _, y in head_pts)
            cv2.line(vis, (0, top_y), (w - 1, top_y), (255, 0, 0), 1, cv2.LINE_AA)
            cv2.putText(vis, "head_anchor_y", (10, max(0, top_y - 8)),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 0, 0), 2)

        # click ratio result
        if st.click_xy and st.ratio is not None and st.bbox:
            cx, cy = st.click_xy
            draw_cross(vis, cx, cy, (255, 255, 255), size=10, thick=2)
            x0, y0, x1, y1 = st.bbox
            bh = max(1, y1 - y0)
            ty = int(y0 + bh * st.ratio)
            cv2.line(vis, (0, ty), (w - 1, ty), (255, 255, 255), 2, cv2.LINE_AA)
            cv2.putText(vis, f"ratio={st.ratio:.4f} target_y={ty}", (10, 80),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 255), 2)

        cv2.putText(vis, "L-click: set 1200mm point | s:save | r:reset | q/esc:quit",
                    (10, h - 12), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2)

        cv2.imshow(win, vis)
        key = cv2.waitKey(30) & 0xFF
        if key in (27, ord("q")):
            break
        if key == ord("r"):
            st.ratio = None
            st.click_xy = None
            print("[ratio] reset")
        if key == ord("s"):
            if not args.out:
                print("[save] --out 경로를 지정해줘")
                continue
            if st.ratio is None or not st.bbox:
                print("[save] ratio가 없음(먼저 클릭)")
                continue
            out_path = str(Path(args.out).expanduser().resolve())
            x0, y0, x1, y1 = st.bbox
            payload = {
                "image": st.img_path,
                "bbox": {"x0": x0, "y0": y0, "x1": x1, "y1": y1, "w": (x1 - x0), "h": (y1 - y0)},
                "ratio_1200mm": st.ratio,
                "click_xy": {"x": st.click_xy[0], "y": st.click_xy[1]} if st.click_xy else None,
                "pose_process_ms": dt_ms,
            }
            Path(out_path).write_text(json.dumps(payload, indent=2, ensure_ascii=False))
            print(f"[save] wrote {out_path}")

    cv2.destroyAllWindows()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

