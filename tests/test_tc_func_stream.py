import os
import socket
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Optional
from urllib.parse import urlparse

import pytest


RTSP_MIN_STABLE_SECONDS = float(os.getenv("SFEPS_STREAM_MIN_STABLE_SECONDS", "10"))
RTSP_RECOVERY_TIMEOUT_SECONDS = float(os.getenv("SFEPS_STREAM_RECOVERY_TIMEOUT_SECONDS", "10"))
RTSP_RECOVERY_POLL_INTERVAL_SECONDS = float(
    os.getenv("SFEPS_STREAM_RECOVERY_POLL_INTERVAL_SECONDS", "1")
)
STREAM_DOWN_DETECTION_TIMEOUT_SECONDS = float(
    os.getenv("SFEPS_STREAM_DOWN_DETECTION_TIMEOUT_SECONDS", "10")
)

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MTX_BIN = str(REPO_ROOT / "mediamtx" / "bin" / "mediamtx")
DEFAULT_MTX_CONFIG = str(REPO_ROOT / "mediamtx" / "mediamtx.yml")
DEFAULT_STREAM_FAULT_DOWN_CMD = (
    "systemctl stop mediamtx "
    "|| sudo -n systemctl stop mediamtx "
    "|| pkill -f '/mediamtx/bin/mediamtx' "
    "|| true"
)
DEFAULT_STREAM_FAULT_UP_CMD = (
    "systemctl start mediamtx "
    "|| sudo -n systemctl start mediamtx "
    f"|| nohup '{REPO_ROOT / 'mediamtx' / 'run_mediamtx.sh'}' >/tmp/sfeps-mediamtx-test.log 2>&1 &"
)

MANAGE_MTX_PROCESS = os.getenv("SFEPS_STREAM_MANAGE_MTX_PROCESS", "0").strip().lower() in (
    "1",
    "true",
    "yes",
    "on",
)
STREAM_FAULT_DOWN_CMD = os.getenv(
    "SFEPS_STREAM_FAULT_DOWN_CMD", DEFAULT_STREAM_FAULT_DOWN_CMD
).strip()
STREAM_FAULT_UP_CMD = os.getenv(
    "SFEPS_STREAM_FAULT_UP_CMD", DEFAULT_STREAM_FAULT_UP_CMD
).strip()
MTX_BIN = os.getenv("SFEPS_STREAM_MTX_BIN", DEFAULT_MTX_BIN).strip()
MTX_CONFIG = os.getenv("SFEPS_STREAM_MTX_CONFIG", DEFAULT_MTX_CONFIG).strip()


@dataclass(frozen=True)
class RtspEndpoint:
    raw_url: str
    host: str
    port: int


class StreamFaultController:
    def down(self) -> None:
        raise NotImplementedError

    def up(self) -> None:
        raise NotImplementedError

    def close(self) -> None:
        return


class CommandFaultController(StreamFaultController):
    def __init__(self, down_cmd: str, up_cmd: str):
        self.down_cmd = down_cmd
        self.up_cmd = up_cmd

    def _run(self, cmd: str, action: str) -> None:
        try:
            subprocess.run(cmd, shell=True, check=True, timeout=30)
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError(f"장애 제어 명령 timeout ({action}): {cmd}") from exc
        except subprocess.CalledProcessError as exc:
            raise RuntimeError(
                f"장애 제어 명령 실패 ({action}), exit={exc.returncode}: {cmd}"
            ) from exc

    def down(self) -> None:
        self._run(self.down_cmd, "down")

    def up(self) -> None:
        self._run(self.up_cmd, "up")


class ManagedMediamtxController(StreamFaultController):
    def __init__(self, bin_path: str, config_path: str):
        self.bin_path = Path(bin_path)
        self.config_path = Path(config_path)
        self.proc: Optional[subprocess.Popen] = None

        if not self.bin_path.is_file():
            raise RuntimeError(f"mediamtx binary not found: {self.bin_path}")
        if not os.access(self.bin_path, os.X_OK):
            raise RuntimeError(f"mediamtx binary is not executable: {self.bin_path}")
        if not self.config_path.is_file():
            raise RuntimeError(f"mediamtx config not found: {self.config_path}")

        self.up()

    def _spawn(self) -> None:
        self.proc = subprocess.Popen(
            [str(self.bin_path), str(self.config_path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(1.0)
        if self.proc.poll() is not None:
            raise RuntimeError(
                f"mediamtx failed to start (exit={self.proc.returncode}): {self.bin_path}"
            )

    def down(self) -> None:
        if self.proc is None or self.proc.poll() is not None:
            return
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)

    def up(self) -> None:
        if self.proc is not None and self.proc.poll() is None:
            return
        self._spawn()

    def close(self) -> None:
        self.down()


def _resolve_rtsp_url() -> str:
    return (
        os.getenv("SFEPS_STREAM_RTSP_URL")
        or os.getenv("RTSP_STREAM_URL")
        or "rtsp://127.0.0.1:8554/cam1"
    )


def _parse_rtsp_endpoint(url: str) -> RtspEndpoint:
    parsed = urlparse(url)
    if parsed.scheme not in ("rtsp", "rtsps"):
        raise ValueError(f"Unsupported RTSP scheme: {parsed.scheme!r}")
    if not parsed.hostname:
        raise ValueError(f"RTSP host is missing in URL: {url}")

    default_port = 554 if parsed.scheme == "rtsp" else 322
    port = parsed.port or default_port
    return RtspEndpoint(raw_url=url, host=parsed.hostname, port=port)


def _recv_rtsp_response(sock: socket.socket) -> str:
    data = b""
    header_end = b"\r\n\r\n"

    while header_end not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
        if len(data) > 256 * 1024:
            break

    if header_end not in data:
        return data.decode("utf-8", errors="replace")

    header_blob, body = data.split(header_end, 1)
    headers = header_blob.decode("utf-8", errors="replace").splitlines()
    content_length = 0
    for line in headers:
        if line.lower().startswith("content-length:"):
            try:
                content_length = int(line.split(":", 1)[1].strip())
            except ValueError:
                content_length = 0
            break

    while content_length > 0 and len(body) < content_length:
        chunk = sock.recv(4096)
        if not chunk:
            break
        body += chunk

    return (header_blob + header_end + body).decode("utf-8", errors="replace")


def _rtsp_request(
    endpoint: RtspEndpoint,
    method: str,
    cseq: int,
    timeout: float = 3.0,
    extra_headers: Optional[Dict[str, str]] = None,
) -> str:
    headers = {
        "CSeq": str(cseq),
        "User-Agent": "SFEPS-PyTest",
    }
    if extra_headers:
        headers.update(extra_headers)

    request = f"{method} {endpoint.raw_url} RTSP/1.0\r\n"
    request += "".join(f"{k}: {v}\r\n" for k, v in headers.items())
    request += "\r\n"

    with socket.create_connection((endpoint.host, endpoint.port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(request.encode("utf-8"))
        return _recv_rtsp_response(sock)


def _rtsp_status_code(response_text: str) -> int:
    first_line = response_text.splitlines()[0] if response_text.splitlines() else ""
    parts = first_line.split()
    if len(parts) < 2:
        return -1
    try:
        return int(parts[1])
    except ValueError:
        return -1


def _is_stream_ready(endpoint: RtspEndpoint, timeout: float = 3.0) -> bool:
    try:
        describe = _rtsp_request(
            endpoint,
            method="DESCRIBE",
            cseq=1,
            timeout=timeout,
            extra_headers={"Accept": "application/sdp"},
        )
    except OSError:
        return False

    return _rtsp_status_code(describe) == 200 and ("m=video" in describe)


def _wait_until(predicate, timeout: float, interval: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return False


@pytest.fixture(scope="module")
def stream_endpoint() -> RtspEndpoint:
    return _parse_rtsp_endpoint(_resolve_rtsp_url())


@pytest.fixture(scope="module")
def stream_fault_controller() -> Optional[StreamFaultController]:
    controller: Optional[StreamFaultController] = None
    try:
        if MANAGE_MTX_PROCESS:
            try:
                controller = ManagedMediamtxController(MTX_BIN, MTX_CONFIG)
            except RuntimeError as exc:
                pytest.skip(f"mediamtx 제어 모드 초기화 실패: {exc}")
        elif STREAM_FAULT_DOWN_CMD and STREAM_FAULT_UP_CMD:
            controller = CommandFaultController(STREAM_FAULT_DOWN_CMD, STREAM_FAULT_UP_CMD)
        yield controller
    finally:
        if controller is not None:
            controller.close()


def test_tc_func_stream_01_receive_stream_by_direct_rtsp_requests(stream_endpoint: RtspEndpoint):
    if not _is_stream_ready(stream_endpoint, timeout=3.0):
        pytest.skip(
            "TC-FUNC-STREAM-01 pre-condition 미충족: "
            f"DESCRIBE 200 + video 트랙 확인 실패 ({stream_endpoint.raw_url})"
        )

    first_options = _rtsp_request(stream_endpoint, method="OPTIONS", cseq=1, timeout=3.0)
    assert _rtsp_status_code(first_options) == 200, "RTSP OPTIONS 실패"

    describe = _rtsp_request(
        stream_endpoint,
        method="DESCRIBE",
        cseq=2,
        timeout=3.0,
        extra_headers={"Accept": "application/sdp"},
    )
    assert _rtsp_status_code(describe) == 200, "RTSP DESCRIBE 실패"
    assert "m=video" in describe, "DESCRIBE 응답 SDP에 video 트랙 정보가 없음"

    deadline = time.monotonic() + RTSP_MIN_STABLE_SECONDS
    cseq = 3
    while time.monotonic() < deadline:
        resp = _rtsp_request(stream_endpoint, method="OPTIONS", cseq=cseq, timeout=3.0)
        assert _rtsp_status_code(resp) == 200, "스트림 유지 구간 중 RTSP 응답 실패"
        cseq += 1
        time.sleep(1.0)


def test_tc_func_stream_03_recover_stream_by_reconnecting_to_live_endpoint(
    stream_endpoint: RtspEndpoint, stream_fault_controller: Optional[StreamFaultController]
):
    if not _is_stream_ready(stream_endpoint, timeout=3.0):
        pytest.skip(
            "TC-FUNC-STREAM-03 pre-condition 미충족: "
            "TC-FUNC-STREAM-01 정상 스트림 상태가 먼저 필요합니다."
        )

    if stream_fault_controller is None:
        pytest.skip(
            "TC-FUNC-STREAM-03 실행 불가: 실제 장애 유도 제어가 설정되지 않았습니다. "
            "SFEPS_STREAM_MANAGE_MTX_PROCESS=1 또는 "
            "SFEPS_STREAM_FAULT_DOWN_CMD/SFEPS_STREAM_FAULT_UP_CMD를 설정하세요."
        )

    recovery_up_error: Optional[Exception] = None
    try:
        try:
            stream_fault_controller.down()
        except Exception as exc:
            pytest.skip(f"실제 장애 유도 명령 실행 실패(down): {exc}")
        down_detected = _wait_until(
            lambda: not _is_stream_ready(stream_endpoint, timeout=2.0),
            STREAM_DOWN_DETECTION_TIMEOUT_SECONDS,
            1.0,
        )
        assert down_detected, "실제 장애 유도 후에도 스트림이 계속 정상 응답합니다."
    finally:
        try:
            stream_fault_controller.up()
        except Exception as exc:  # pragma: no cover
            recovery_up_error = exc

    assert recovery_up_error is None, f"장애 복구(UP) 명령 실패: {recovery_up_error}"

    recovered = _wait_until(
        lambda: _is_stream_ready(stream_endpoint, timeout=3.0),
        RTSP_RECOVERY_TIMEOUT_SECONDS,
        RTSP_RECOVERY_POLL_INTERVAL_SECONDS,
    )
    assert recovered, (
        "복구 시간 내 정상 엔드포인트 재연결 실패: "
        f"url={stream_endpoint.raw_url}, timeout={RTSP_RECOVERY_TIMEOUT_SECONDS}s"
    )
