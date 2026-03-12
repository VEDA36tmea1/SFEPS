#!/usr/bin/env python3
"""
plane_points.json 을 읽어서 OpenCV Homography H를 계산하고
homography.yml (OpenCV FileStorage) 로 저장.
rtsp_laser_demo --world-track 에서 이 파일을 로드해 픽셀 → (X,Y) mm 변환에 사용.

사용:
  cd Camera/calibration
  python3 export_homography.py
  python3 export_homography.py plane_points.json homography.yml
"""

import json
import os
import sys

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    json_path = sys.argv[1] if len(sys.argv) >= 2 else os.path.join(HERE, "plane_points.json")
    yaml_path = sys.argv[2] if len(sys.argv) >= 3 else os.path.join(HERE, "homography.yml")

    if not os.path.isabs(json_path):
        json_path = os.path.join(HERE, json_path)
    if not os.path.exists(json_path):
        print(f"[ERROR] 파일 없음: {json_path}", file=sys.stderr)
        sys.exit(1)

    with open(json_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    pix = data.get("pixel_points", [])
    world = data.get("world_points", [])
    if len(pix) < 4 or len(world) < 4:
        print("[ERROR] pixel_points / world_points 최소 4개 필요", file=sys.stderr)
        sys.exit(1)

    src_pts = np.array([[p["x"], p["y"]] for p in pix[:4]], dtype=np.float64)
    dst_pts = np.array([[p["X"], p["Y"]] for p in world[:4]], dtype=np.float64)

    H, _ = cv2.findHomography(src_pts, dst_pts)
    if H is None:
        print("[ERROR] findHomography 실패", file=sys.stderr)
        sys.exit(1)

    # OpenCV FileStorage로 저장 (C++ cv::FileStorage에서 "H" 키로 읽음)
    cv2.FileStorage(yaml_path, cv2.FILE_STORAGE_WRITE).write("H", H).release()

    print(f"[OK] {yaml_path} 저장 (H 3x3)")
    print("rtsp_laser_demo 실행 시: --world-track [homography.yml 경로]")


if __name__ == "__main__":
    main()
