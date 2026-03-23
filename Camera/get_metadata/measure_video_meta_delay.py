#!/usr/bin/env python3
"""
Quick method-A delay probe:
- video_ts: local receive time when OpenCV reads RTSP frame
- meta_ts: local receive time when ONVIF metadata XML block is parsed
- delay_ms ~= video_ts - meta_ts (using latest video frame timestamp)
"""

import argparse
import os
import re
import socket
import statistics
import threading
import time
from collections import defaultdict, deque

import cv2


OBJECT_RE = re.compile(r"<tt:Object\b[^>]*ObjectId=\"([^\"]+)\"[^>]*>(.*?)</tt:Object>", re.DOTALL)
TYPE_RE = re.compile(r"<tt:Type>([^<]+)</tt:Type>")


def parse_human_ids(xml_text: str):
    ids = []
    for obj_id, body in OBJECT_RE.findall(xml_text):
        t = TYPE_RE.search(body)
        if t and t.group(1).strip().lower() == "human":
            ids.append(obj_id.strip())
    return ids


def recv_exact(sock: socket.socket, n: int):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def rtsp_handshake(sock: socket.socket, rtsp_url: str):
    cseq = 1

    def send_req(req: str):
        nonlocal cseq
        data = req.replace("{CSEQ}", str(cseq)).encode("utf-8")
        sock.sendall(data)
        resp = sock.recv(8192).decode("utf-8", errors="ignore")
        cseq += 1
        return resp

    send_req(
        f"OPTIONS {rtsp_url} RTSP/1.0\r\nCSeq: {{CSEQ}}\r\nUser-Agent: DelayProbe\r\n\r\n"
    )
    send_req(
        f"DESCRIBE {rtsp_url} RTSP/1.0\r\nCSeq: {{CSEQ}}\r\nAccept: application/sdp\r\nUser-Agent: DelayProbe\r\n\r\n"
    )
    resp_setup_v = send_req(
        f"SETUP {rtsp_url}/trackID=v RTSP/1.0\r\nCSeq: {{CSEQ}}\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nUser-Agent: DelayProbe\r\n\r\n"
    )

    m = re.search(r"Session:\s*([^\r\n;]+)", resp_setup_v)
    if not m:
        raise RuntimeError("Session ID not found")
    session_id = m.group(1).strip()

    send_req(
        f"SETUP {rtsp_url}/trackID=m RTSP/1.0\r\nCSeq: {{CSEQ}}\r\nTransport: RTP/AVP/TCP;unicast;interleaved=2-3\r\nSession: {session_id}\r\nUser-Agent: DelayProbe\r\n\r\n"
    )
    send_req(
        f"PLAY {rtsp_url} RTSP/1.0\r\nCSeq: {{CSEQ}}\r\nSession: {session_id}\r\nRange: npt=0.000-\r\nUser-Agent: DelayProbe\r\n\r\n"
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--camera-ip", default="192.168.0.84")
    ap.add_argument("--camera-port", type=int, default=554)
    ap.add_argument("--rtsp-url", default="rtsp://192.168.0.84/profile2/media.smp")
    ap.add_argument("--seconds", type=int, default=20)
    args = ap.parse_args()

    os.environ["OPENCV_FFMPEG_CAPTURE_OPTIONS"] = (
        "rtsp_transport;tcp|fflags;nobuffer|flags;low_delay|max_delay;0|"
        "probesize;32768|analyzeduration;0|reorder_queue_size;0"
    )

    stop = threading.Event()
    latest_video_ts_ns = {"value": 0}
    delays = deque(maxlen=5000)
    delays_by_id = defaultdict(lambda: deque(maxlen=500))

    def video_thread():
        cap = cv2.VideoCapture(args.rtsp_url, cv2.CAP_FFMPEG)
        if not cap.isOpened():
            print("[video] failed to open:", args.rtsp_url)
            stop.set()
            return
        while not stop.is_set():
            ok, _ = cap.read()
            if not ok:
                time.sleep(0.01)
                continue
            latest_video_ts_ns["value"] = time.monotonic_ns()
        cap.release()

    def meta_thread():
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(2.0)
        sock.connect((args.camera_ip, args.camera_port))
        rtsp_handshake(sock, args.rtsp_url)
        sock.settimeout(1.0)

        acc_xml = ""
        last_ts = 0
        while not stop.is_set():
            try:
                hdr = recv_exact(sock, 4)
                if not hdr:
                    break
                if hdr[0] != 0x24:  # '$'
                    continue
                channel = hdr[1]
                plen = (hdr[2] << 8) | hdr[3]
                payload = recv_exact(sock, plen)
                if not payload or len(payload) <= 12:
                    continue
                if channel != 2:
                    continue
                rtp_ts = int.from_bytes(payload[4:8], "big", signed=False)
                xml_part = payload[12:].decode("utf-8", errors="ignore")
                if rtp_ts != last_ts and last_ts != 0:
                    meta_ts_ns = time.monotonic_ns()
                    ids = parse_human_ids(acc_xml)
                    vts = latest_video_ts_ns["value"]
                    if vts > 0:
                        delay_ms = (vts - meta_ts_ns) / 1_000_000.0
                        delays.append(delay_ms)
                        for oid in ids:
                            delays_by_id[oid].append(delay_ms)
                    acc_xml = ""
                acc_xml += xml_part
                last_ts = rtp_ts
            except socket.timeout:
                continue
            except Exception as e:
                print("[meta] error:", e)
                break
        sock.close()
        stop.set()

    vt = threading.Thread(target=video_thread, daemon=True)
    mt = threading.Thread(target=meta_thread, daemon=True)
    vt.start()
    mt.start()

    t0 = time.time()
    while not stop.is_set() and (time.time() - t0) < args.seconds:
        time.sleep(1.0)
        if delays:
            arr = list(delays)
            p50 = statistics.median(arr)
            p95 = sorted(arr)[int(len(arr) * 0.95) - 1] if len(arr) > 1 else arr[0]
            print(
                f"[live] samples={len(arr)} avg={statistics.fmean(arr):.1f}ms "
                f"p50={p50:.1f}ms p95={p95:.1f}ms latest={arr[-1]:.1f}ms"
            )
        else:
            print("[live] waiting samples...")

    stop.set()
    vt.join(timeout=1.0)
    mt.join(timeout=1.0)

    if not delays:
        print("[result] no samples collected")
        return

    arr = list(delays)
    print(
        f"[result] total={len(arr)} avg={statistics.fmean(arr):.2f}ms "
        f"p50={statistics.median(arr):.2f}ms "
        f"p95={sorted(arr)[int(len(arr) * 0.95) - 1]:.2f}ms "
        f"min={min(arr):.2f}ms max={max(arr):.2f}ms"
    )
    top_ids = sorted(delays_by_id.items(), key=lambda kv: len(kv[1]), reverse=True)[:5]
    for oid, q in top_ids:
        vals = list(q)
        print(f"[id={oid}] n={len(vals)} avg={statistics.fmean(vals):.2f}ms latest={vals[-1]:.2f}ms")


if __name__ == "__main__":
    main()

