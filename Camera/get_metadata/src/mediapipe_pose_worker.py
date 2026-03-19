#!/usr/bin/env python3
"""
stdin/stdout 기반 MediaPipe Pose worker.

IPC 프로토콜:
- C++ -> Python
  - 4바이트 little-endian uint32: JPEG 바이트 길이 N
  - 이어서 N바이트 JPEG 데이터
- Python -> C++
  - 한 줄 문자열(뉴라인 포함):
    - "0" (pose not found / decode failed)
    - 또는 "1 x0 y0 v0 x1 y1 v1 ... x32 y32 v32"
      (x,y는 ROI 이미지 기준 정규화 좌표[0,1], v는 visibility)

실시간 프레임마다 돌리면 무거울 수 있으니,
C++에서 일정 프레임마다(예: 5프레임에 1회) 호출하는 걸 추천합니다.
"""

import os
import struct
import sys

import cv2
import mediapipe as mp
import numpy as np

os.environ.setdefault("GLOG_minloglevel", "3")
os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "3")


def read_exact(n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sys.stdin.buffer.read(n - len(buf))
        if not chunk:
            return bytes(buf)
        buf.extend(chunk)
    return bytes(buf)


mp_pose = mp.solutions.pose
pose = mp_pose.Pose(
    static_image_mode=False,
    model_complexity=1,
    enable_segmentation=False,
    min_detection_confidence=0.5,
    min_tracking_confidence=0.5,
)


def process_jpeg(jpeg_bytes: bytes) -> str:
    arr = np.frombuffer(jpeg_bytes, dtype=np.uint8)
    img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
    if img is None or img.size == 0:
        return "0\n"

    rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
    res = pose.process(rgb)
    if not res.pose_landmarks:
        return "0\n"

    lms = res.pose_landmarks.landmark
    # mediapipe pose는 33 landmarks
    toks = ["1"]
    for lm in lms:
        toks.append(f"{lm.x:.6f}")
        toks.append(f"{lm.y:.6f}")
        toks.append(f"{lm.visibility:.6f}")
    return " ".join(toks) + "\n"


def main() -> None:
    while True:
        hdr = sys.stdin.buffer.read(4)
        if not hdr:
            break
        if len(hdr) < 4:
            break
        (n,) = struct.unpack("<I", hdr)
        if n <= 0:
            sys.stdout.write("0\n")
            sys.stdout.flush()
            continue
        jpeg = read_exact(n)
        if len(jpeg) != n:
            break

        out = process_jpeg(jpeg)
        sys.stdout.write(out)
        sys.stdout.flush()


if __name__ == "__main__":
    main()

