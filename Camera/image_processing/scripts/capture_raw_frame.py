#!/usr/bin/env python3

import argparse
import os
import signal
import socket
import sys
import time
from typing import Dict, List

from picamera2 import Picamera2


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Persistent RAW Bayer capture helper")
    parser.add_argument("--server", action="store_true")
    parser.add_argument("--socket-path")
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--warmup-ms", type=int, default=250)
    parser.add_argument("--shutter-us", type=int, default=0)
    parser.add_argument("--gain", type=float, default=0.0)
    return parser.parse_args()


def get_mode_candidates(picam2: Picamera2, width: int, height: int) -> List[Dict]:
    modes = []
    for mode in picam2.sensor_modes:
        size = tuple(mode.get("size", (0, 0)))
        pixel_format = str(mode.get("format", ""))
        unpacked_format = mode.get("unpacked", pixel_format)
        modes.append(
            {
                "size": size,
                "format": pixel_format,
                "unpacked": unpacked_format,
                "bit_depth": int(mode.get("bit_depth", 0)),
            }
        )

    exact = [m for m in modes if m["size"] == (width, height)]
    ranked = exact if exact else modes
    ranked.sort(
        key=lambda m: (
            0 if m["size"] == (width, height) else 1,
            0 if m["bit_depth"] == 10 else 1,
            0 if "_CSI2P" not in m["unpacked"] else 1,
        )
    )
    return ranked


def unique_formats(mode: Dict) -> List[str]:
    out: List[str] = []
    for raw_format in (mode["unpacked"], mode["format"]):
        if raw_format and raw_format not in out:
            out.append(raw_format)
    return out


def write_metadata(path: str, values: Dict[str, str]) -> None:
    with open(path, "w", encoding="utf-8") as handle:
        for key, value in values.items():
            handle.write(f"{key}={value}\n")


class RawCaptureServer:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.picam2 = Picamera2()
        self.mode = None
        self.raw_format = None
        self.server = None
        self.running = True
        signal.signal(signal.SIGTERM, self._handle_stop)
        signal.signal(signal.SIGINT, self._handle_stop)

    def _handle_stop(self, *_args):
        self.running = False
        if self.server is not None:
            try:
                self.server.close()
            except Exception:
                pass

    @staticmethod
    def _safe_send(conn: socket.socket, payload: bytes) -> None:
        try:
            conn.sendall(payload)
        except BrokenPipeError:
            pass
        except OSError:
            pass

    def configure(self) -> None:
        failures = []
        for mode in get_mode_candidates(self.picam2, self.args.width, self.args.height):
            for raw_format in unique_formats(mode):
                try:
                    controls = {}
                    if self.args.shutter_us > 0:
                        controls["ExposureTime"] = self.args.shutter_us
                    if self.args.gain > 0.0:
                        controls["AnalogueGain"] = self.args.gain

                    config = self.picam2.create_still_configuration(
                        main={"format": "BGR888", "size": (640, 480)},
                        raw={"format": raw_format, "size": mode["size"]},
                        display=None,
                        encode=None,
                        queue=False,
                        controls=controls,
                    )
                    self.picam2.configure(config)
                    self.picam2.start()
                    if self.args.warmup_ms > 0:
                        time.sleep(self.args.warmup_ms / 1000.0)
                    self.mode = mode
                    self.raw_format = raw_format
                    print(
                        f"[raw-helper] ready format={raw_format} size={mode['size'][0]}x{mode['size'][1]}",
                        flush=True,
                    )
                    return
                except Exception as exc:
                    failures.append(
                        f"mode={mode['size'][0]}x{mode['size'][1]} format={raw_format}: {exc}"
                    )
                    try:
                        self.picam2.stop()
                    except Exception:
                        pass

        raise RuntimeError("; ".join(failures) if failures else "no RAW mode available")

    def capture_to_files(self, raw_path: str, meta_path: str) -> None:
        arrays, metadata = self.picam2.capture_arrays(["raw"])
        raw = arrays[0]
        stride = int(raw.shape[1]) if len(raw.shape) >= 2 else int(raw.size)
        raw.tofile(raw_path)
        write_metadata(
            meta_path,
            {
                "FORMAT": self.raw_format,
                "WIDTH": str(self.mode["size"][0]),
                "HEIGHT": str(self.mode["size"][1]),
                "STRIDE": str(stride),
                "BIT_DEPTH": str(self.mode["bit_depth"]),
                "EXPOSURE_TIME": str(metadata.get("ExposureTime", "")),
                "ANALOGUE_GAIN": str(metadata.get("AnalogueGain", "")),
            },
        )

    def serve(self) -> int:
        if not self.args.socket_path:
            raise RuntimeError("--socket-path is required in --server mode")

        self.configure()

        try:
            os.unlink(self.args.socket_path)
        except FileNotFoundError:
            pass

        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server.bind(self.args.socket_path)
        self.server.listen(4)
        self.server.settimeout(0.5)

        try:
            while self.running:
                try:
                    conn, _ = self.server.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break

                with conn:
                    try:
                        line = conn.recv(4096).decode("utf-8", errors="replace").strip()
                        if not line:
                            continue
                        if line == "PING":
                            self._safe_send(conn, b"OK\n")
                            continue
                        if line == "STOP":
                            self._safe_send(conn, b"OK\n")
                            self.running = False
                            continue

                        parts = {}
                        for token in line.split("|"):
                            if "=" not in token:
                                continue
                            key, value = token.split("=", 1)
                            parts[key] = value

                        raw_path = parts.get("RAW")
                        meta_path = parts.get("META")
                        if not raw_path or not meta_path:
                            self._safe_send(conn, b"ERR|missing RAW/META path\n")
                            continue

                        self.capture_to_files(raw_path, meta_path)
                        self._safe_send(conn, b"OK\n")
                    except Exception as exc:
                        msg = f"ERR|{exc}\n".encode("utf-8", errors="replace")
                        self._safe_send(conn, msg)
        finally:
            try:
                self.picam2.stop()
            except Exception:
                pass
            self.picam2.close()
            try:
                self.server.close()
            except Exception:
                pass
            try:
                os.unlink(self.args.socket_path)
            except Exception:
                pass

        return 0


def main() -> int:
    args = parse_args()
    if not args.server:
        print("--server mode is required", file=sys.stderr)
        return 2
    return RawCaptureServer(args).serve()


if __name__ == "__main__":
    raise SystemExit(main())
