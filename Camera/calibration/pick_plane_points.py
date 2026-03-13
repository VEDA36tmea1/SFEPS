#!/usr/bin/env python3
"""
이미지 위에서 4개 점을 클릭하고,
각 점 사이의 실제 평면 거리/좌표를 터미널에서 입력해 JSON으로 저장하는 도구.

기본 동작:
- ref_lines.json 이 있으면 그 안의 image_path 를 기본 이미지로 사용.
- 없으면 첫 번째 인자로 전달한 이미지 경로를 사용.

조작:
- 마우스 왼쪽 클릭: 점 추가 (최대 4개)
- 마우스 오른쪽 클릭: 마지막 점 삭제 (실수 방지용)
- 4개 찍은 뒤 터미널로 돌아가서, 안내에 따라 각 점의 실제 (X, Y) 좌표를 입력
  (예: mm 또는 m 단위 / 첫 점을 (0,0)으로 두고 나머지 거리 입력 등)
- 완료 후 plane_points.json 에 저장
"""

from __future__ import annotations

import json
import os
import sys
from typing import List, Tuple, Any, Dict

import cv2
import numpy as np


HERE = os.path.dirname(os.path.abspath(__file__))
REF_JSON_PATH = os.path.join(HERE, "ref_lines.json")


def load_default_image_path(cli_arg: str | None) -> str:
    # 1순위: CLI 인자
    if cli_arg:
        return cli_arg if os.path.isabs(cli_arg) else os.path.join(HERE, cli_arg)

    # 2순위: ref_lines.json 의 image_path
    if os.path.exists(REF_JSON_PATH):
        try:
            with open(REF_JSON_PATH, "r", encoding="utf-8") as f:
                data = json.load(f)
            img_path = data.get("image_path") or data.get("image")
            if img_path:
                if not os.path.isabs(img_path):
                    img_path = os.path.join(HERE, img_path)
                return img_path
        except Exception:
            pass

    # 3순위: 기본 파일명
    return os.path.join(HERE, "75.png")


def main() -> None:
    img_arg = sys.argv[1] if len(sys.argv) >= 2 else None
    img_path = load_default_image_path(img_arg)

    if not os.path.exists(img_path):
        print(f"[ERROR] 이미지 없음: {img_path}", file=sys.stderr)
        sys.exit(1)

    img0 = cv2.imread(img_path)
    if img0 is None:
        print(f"[ERROR] imread 실패: {img_path}", file=sys.stderr)
        sys.exit(1)

    vis = img0.copy()
    points: List[Tuple[int, int]] = []

    win = "pick 4 plane points (L: add, R: undo, ENTER in terminal after)"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)

    def redraw() -> None:
        nonlocal vis
        vis = img0.copy()
        for i, (x, y) in enumerate(points):
            cv2.circle(vis, (x, y), 6, (0, 255, 0), -1, cv2.LINE_AA)
            cv2.putText(
                vis,
                f"P{i+1}",
                (x + 8, y - 8),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.8,
                (0, 255, 0),
                2,
            )
        cv2.imshow(win, vis)

    def on_mouse(event: int, x: int, y: int, flags: int, param: Any) -> None:
        nonlocal points
        if event == cv2.EVENT_LBUTTONDOWN:
            if len(points) < 4:
                points.append((x, y))
                redraw()
        elif event == cv2.EVENT_RBUTTONDOWN:
            if points:
                points.pop()
                redraw()

    cv2.setMouseCallback(win, on_mouse)
    redraw()

    print(f"[INFO] 이미지: {img_path}")
    print("[INFO] 마우스 왼쪽 클릭으로 점을 찍고, 오른쪽 클릭으로 마지막 점을 취소할 수 있습니다.")
    print("[INFO] 총 4개 점을 찍으세요. (찍으면서 화면에 P1~P4 라벨이 뜹니다)")
    print("[INFO] 4개를 찍은 뒤, 터미널로 돌아와 엔터를 치면 좌표 입력 단계로 넘어갑니다.\n")

    # GUI 루프: 4점이 찍힐 때까지 대기
    while True:
        cv2.imshow(win, vis)
        key = cv2.waitKey(20) & 0xFF
        if key in (27, ord("q")):
            cv2.destroyAllWindows()
            sys.exit(0)
        if len(points) >= 4:
            # 4개를 다 찍으면 안내만 띄우고, 터미널 입력을 기다리게 함
            cv2.putText(
                vis,
                "4 points selected. Go to terminal.",
                (10, 30),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.8,
                (0, 255, 255),
                2,
            )
            cv2.imshow(win, vis)
            break

    input("\n[터미널] 4개 점 선택 완료. 엔터를 누르면 각 점의 실제 (X, Y) 평면 좌표를 입력합니다...")

    world_points: List[Tuple[float, float]] = []
    print(
        "\n각 점의 실제 평면 좌표 (X Y)를 입력하세요."
        "\n예: 첫 점을 원점으로 두고 싶으면 P1에 '0 0',"
        "\n    P2에 '1000 0' (1m), P3에 '1000 500' (1m,0.5m) 등으로 입력."
    )
    for i, (x, y) in enumerate(points):
        while True:
            s = input(f"P{i+1} (pixel=({x},{y})) 의 세계 좌표 X Y: ").strip()
            if not s:
                print("빈 입력입니다. 예: 0 0 또는 1000 0 처럼 입력하세요.")
                continue
            parts = s.replace(",", " ").split()
            if len(parts) != 2:
                print("X Y 두 개 값을 공백으로 구분해 주세요. 예: 0 0")
                continue
            try:
                X = float(parts[0])
                Y = float(parts[1])
            except ValueError:
                print("숫자로 해석할 수 없습니다. 예: 0 0 또는 1000 0")
                continue
            world_points.append((X, Y))
            break

    out_path = os.path.join(HERE, "plane_points.json")
    payload: Dict[str, Any] = {
        "image": os.path.basename(img_path),
        "image_path": img_path,
        "pixel_points": [{"x": int(x), "y": int(y)} for (x, y) in points],
        "world_points": [{"X": float(X), "Y": float(Y)} for (X, Y) in world_points],
        "note": "world_points 단위는 사용자가 입력한 단위(mm/m 등)와 동일합니다.",
    }

    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2, ensure_ascii=False)

    print(f"\n[OK] plane_points.json 저장 완료: {out_path}")
    print("내용 예시:")
    print(json.dumps(payload, indent=2, ensure_ascii=False))

    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()

