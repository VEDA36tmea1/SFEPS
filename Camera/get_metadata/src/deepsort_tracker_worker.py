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

수정 이력:
- n_init 1→3  : 3프레임 연속 확인돼야 confirmed → 노이즈 1프레임 bbox 튐 제거
- max_cosine_distance 0.3→0.6 : 화면 밖 복귀 시 재매칭 허용범위 확대
- nn_budget=300 : 최근 300개 embedding 보관 → 복귀 ReID 매칭 품질 향상
- ReID 버퍼 추가 : max_age 이상 사라졌다가 재등장해도 동일 ID 재발급
"""

import os
import struct
import sys
from collections import deque

import cv2
import numpy as np
from deep_sort_realtime.deepsort_tracker import DeepSort

os.environ.setdefault("GLOG_minloglevel", "3")
os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "3")

# ── DeepSORT 초기화 ──────────────────────────────────────────────────
# [수정1] n_init=3  : 1→3. 1프레임만 나타나도 confirmed가 되던 문제 수정.
#         노이즈 detection이 잠깐 튀어도 bbox가 순간 점프하지 않는다.
# [수정2] max_cosine_distance=0.6 : 0.3→0.6. 외형 매칭 허용범위 확대.
#         화면 밖에서 각도/조명이 살짝 달라진 채 복귀해도 재매칭 성공률 ↑
# [수정3] nn_budget=300 : 최근 300개 embedding을 기억해 오래된 재등장도 매칭 가능
tracker = DeepSort(
    max_age=100,
    n_init=3,
    embedder="mobilenet",
    half=False,
    bgr=True,
    max_iou_distance=0.85,
    max_cosine_distance=0.5,
    embedder_gpu=False,
    nn_budget=150,
)

# ── ReID 버퍼 ────────────────────────────────────────────────────────
# DeepSort max_age(90프레임)를 넘어 track이 삭제된 뒤에도
# REID_MAX_AGE 프레임 이내 재등장하면 동일 ID를 재발급한다.
# 구조: { original_id(int) : {"embs": deque, "ltrb": list, "frame_no": int} }
REID_MAX_AGE    = 600    # 프레임 수 (약 20초 @ 30fps)
REID_COS_THRESH = 0.55   # cosine distance 임계값 (낮을수록 엄격)

reid_buffer: dict = {}
id_remap:    dict = {}   # new_ds_id → original_id
frame_no:    int  = 0
prev_confirmed_ids: set = set()


def _cosine_dist(a: np.ndarray, b: np.ndarray) -> float:
    na = np.linalg.norm(a)
    nb = np.linalg.norm(b)
    if na < 1e-9 or nb < 1e-9:
        return 1.0
    return float(1.0 - np.dot(a, b) / (na * nb))


def _mean_emb(embs) -> np.ndarray:
    arr = np.stack(list(embs), axis=0)
    m   = arr.mean(axis=0)
    n   = np.linalg.norm(m)
    return m / (n + 1e-9)


def _get_embedding(track) -> "np.ndarray | None":
    """deep_sort_realtime 버전마다 embedding 접근 경로가 다르므로 순서대로 시도."""
    if hasattr(track, "features") and track.features:
        try:
            return np.asarray(track.features[-1], dtype=np.float32)
        except Exception:
            pass
    if hasattr(track, "get_det_supplementary"):
        try:
            sup = track.get_det_supplementary()
            if sup is not None:
                return np.asarray(sup, dtype=np.float32)
        except Exception:
            pass
    if hasattr(track, "_det_supplementary") and track._det_supplementary is not None:
        try:
            return np.asarray(track._det_supplementary, dtype=np.float32)
        except Exception:
            pass
    return None


def read_exact(n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sys.stdin.buffer.read(n - len(buf))
        if not chunk:
            return bytes(buf)
        buf.extend(chunk)
    return bytes(buf)


def process(jpeg_bytes: bytes, raw_bboxes: list) -> str:
    global frame_no, prev_confirmed_ids

    arr   = np.frombuffer(jpeg_bytes, dtype=np.uint8)
    frame = cv2.imdecode(arr, cv2.IMREAD_COLOR)
    if frame is None or frame.size == 0:
        return "0\n"
    if not raw_bboxes:
        return "0\n"

    frame_no += 1

    # DeepSort 입력 포맷: [[left, top, w, h], confidence, class_id]
    detections = [([l, t, r - l, b - t], conf, 0) for (l, t, r, b, conf) in raw_bboxes]
    try:
        tracks = tracker.update_tracks(detections, frame=frame)
    except Exception as e:
        # Never let worker die from runtime tracking exceptions.
        sys.stderr.write(f"[deepsort_worker] update_tracks error: {e}\n")
        sys.stderr.flush()
        return "0\n"

    current_confirmed_ids: set = {tr.track_id for tr in tracks if tr.is_confirmed()}

    # ── 방금 화면에서 사라진 track → ReID 버퍼 저장 ─────────────
    disappeared = prev_confirmed_ids - current_confirmed_ids
    for tr in tracks:
        if tr.track_id not in disappeared:
            continue
        emb = _get_embedding(tr)
        if emb is None:
            continue
        oid = id_remap.get(tr.track_id, tr.track_id)
        if oid not in reid_buffer:
            reid_buffer[oid] = {
                "embs":     deque(maxlen=30),
                "ltrb":     tr.to_ltrb().tolist(),
                "frame_no": frame_no,
            }
        reid_buffer[oid]["embs"].append(emb)
        reid_buffer[oid]["frame_no"] = frame_no

    # ── 오래된 ReID 버퍼 항목 제거 ───────────────────────────────
    for oid in [k for k, v in reid_buffer.items()
                if frame_no - v["frame_no"] > REID_MAX_AGE]:
        del reid_buffer[oid]

    # ── 새로 confirmed된 track → ReID 버퍼와 매칭 ───────────────
    newly_confirmed = current_confirmed_ids - prev_confirmed_ids
    for tr in tracks:
        tid = tr.track_id
        if tid not in newly_confirmed or tid in id_remap:
            continue
        emb = _get_embedding(tr)
        if emb is None or not reid_buffer:
            continue

        best_oid   = None
        best_score = REID_COS_THRESH
        for oid, buf in reid_buffer.items():
            if not buf["embs"]:
                continue
            dist = _cosine_dist(emb, _mean_emb(buf["embs"]))
            if dist < best_score:
                best_score = dist
                best_oid   = oid

        if best_oid is not None:
            id_remap[tid] = best_oid
            del reid_buffer[best_oid]
            sys.stderr.write(
                f"[ReID] ds_id={tid} → original_id={best_oid}"
                f" (cos={best_score:.3f})\n"
            )
            sys.stderr.flush()

    prev_confirmed_ids = current_confirmed_ids

    # ── 결과 조립 ─────────────────────────────────────────────────
    results = []
    for tr in tracks:
        if not tr.is_confirmed():
            continue
        final_id = id_remap.get(tr.track_id, tr.track_id)
        ltrb = tr.to_ltrb()
        l, t, r, b = int(ltrb[0]), int(ltrb[1]), int(ltrb[2]), int(ltrb[3])
        results.append(f"{final_id} {l} {t} {r} {b}")

    if not results:
        return "0\n"
    return f"{len(results)} " + " ".join(results) + "\n"


def main() -> None:
    while True:
        try:
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

            bbox_hdr = sys.stdin.buffer.read(4)
            if not bbox_hdr or len(bbox_hdr) < 4:
                break
            (bbox_count,) = struct.unpack("<I", bbox_hdr)

            raw_bboxes = []
            for _ in range(bbox_count):
                raw = sys.stdin.buffer.read(20)
                if len(raw) < 20:
                    break
                l, t, r, b, conf = struct.unpack("<fffff", raw)
                raw_bboxes.append((l, t, r, b, conf))

            sys.stdout.write(process(jpeg, raw_bboxes))
            sys.stdout.flush()
        except Exception as e:
            # Keep worker alive and report one-line failure for this frame.
            sys.stderr.write(f"[deepsort_worker] loop error: {e}\n")
            sys.stderr.flush()
            try:
                sys.stdout.write("0\n")
                sys.stdout.flush()
            except Exception:
                break


if __name__ == "__main__":
    main()