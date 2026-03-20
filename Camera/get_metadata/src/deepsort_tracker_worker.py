#!/usr/bin/env python3
"""
stdin/stdout 기반 DeepSORT 트래킹 워커.

IPC 프로토콜:
- C++ -> Python:
  - 4바이트 little-endian uint32: JPEG 길이 N
  - N바이트 JPEG 데이터
  - 4바이트 little-endian uint32: bbox 개수 M
  - bbox마다 5 × float32 (20바이트): left, top, right, bottom, confidence

- Python -> C++:
  - 한 줄 문자열(뉴라인 포함):
    - "0" (트래킹 실패 / 디코드 실패)
    - 또는 "M track_id0 left0 top0 right0 bottom0 track_id1 left1 ..."
      (M = 트래킹된 객체 수, 좌표는 원본 픽셀 기준)
"""

import os
import struct
import sys

import cv2
import numpy as np
from deep_sort_realtime.deepsort_tracker import DeepSort

os.environ.setdefault("GLOG_minloglevel", "3")
os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "3")

# DeepSORT 초기화
# max_age: 트랙이 사라진 후 몇 프레임까지 유지할지
# n_init: 몇 프레임 연속으로 나타나야 확정 트랙으로 인정할지
# embedder: 외형 특징 추출기 (mobilenet이 라즈베리파이에서 가장 가벼움)
tracker = DeepSort(
    max_age=30,
    n_init=2,
    embedder="mobilenet",
    half=False,         # 라즈베리파이는 FP16 미지원
    bgr=True,           # OpenCV BGR 입력
)

def read_exact(n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sys.stdin.buffer.read(n - len(buf))
        if not chunk:
            return bytes(buf)
        buf.extend(chunk)
    return bytes(buf)

def process(jpeg_bytes: bytes, raw_bboxes: list) -> str:
    """
    jpeg_bytes: 전체 프레임 JPEG
    raw_bboxes: [(left, top, right, bottom, confidence), ...]
    반환: "M id0 l0 t0 r0 b0 id1 l1 t1 r1 b1 ..."
    """
    arr = np.frombuffer(jpeg_bytes, dtype=np.uint8)
    frame = cv2.imdecode(arr, cv2.IMREAD_COLOR)
    if frame is None or frame.size == 0:
        return "0\n"

    if not raw_bboxes:
        return "0\n"

    # DeepSort 입력 포맷: [[left, top, w, h], confidence, class_id]
    detections = []
    for (l, t, r, b, conf) in raw_bboxes:
        w = r - l
        h = b - t
        detections.append(([l, t, w, h], conf, 0))

    tracks = tracker.update_tracks(detections, frame=frame)

    results = []
    for track in tracks:
        if not track.is_confirmed():
            continue
        tid = track.track_id
        ltrb = track.to_ltrb()
        l, t, r, b = int(ltrb[0]), int(ltrb[1]), int(ltrb[2]), int(ltrb[3])
        results.append(f"{tid} {l} {t} {r} {b}")

    if not results:
        return "0\n"

    return f"{len(results)} " + " ".join(results) + "\n"

def main() -> None:
    while True:
        # 1. JPEG 읽기
        hdr = sys.stdin.buffer.read(4)
        if not hdr or len(hdr) < 4:
            break
        (jpeg_len,) = struct.unpack("<I", hdr)

        if jpeg_len == 0:
            sys.stdout.write("0\n")
            sys.stdout.flush()
            continue

        jpeg = read_exact(jpeg_len)
        if len(jpeg) != jpeg_len:
            break

        # 2. bbox 읽기
        bbox_hdr = sys.stdin.buffer.read(4)
        if not bbox_hdr or len(bbox_hdr) < 4:
            break
        (bbox_count,) = struct.unpack("<I", bbox_hdr)

        raw_bboxes = []
        for _ in range(bbox_count):
            raw = sys.stdin.buffer.read(20)  # 5 × float32
            if len(raw) < 20:
                break
            l, t, r, b, conf = struct.unpack("<fffff", raw)
            raw_bboxes.append((l, t, r, b, conf))

        out = process(jpeg, raw_bboxes)
        sys.stdout.write(out)
        sys.stdout.flush()

if __name__ == "__main__":
    main()
