import os
import socket
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Optional
from urllib.parse import urlparse

import pytest


AUTH_HOST = "127.0.0.1"
AUTH_PORT = 5555
ALERT_PORT = 5557
POSITION_PORT = 5558
SERVER_LOG_PATH = Path(__file__).resolve().parent / "real_server.log"


def _env_bool(name: str, default: bool) -> bool:
    raw = os.getenv(name)
    if raw is None:
        return default
    return raw.strip().lower() in ("1", "true", "yes", "on")


def _env_float(name: str, default: float) -> float:
    raw = os.getenv(name)
    if raw is None:
        return default
    try:
        return float(raw)
    except ValueError as exc:
        raise ValueError(f"{name} must be float, got {raw!r}") from exc


def _env_int(name: str, default: int) -> int:
    raw = os.getenv(name)
    if raw is None:
        return default
    try:
        return int(raw)
    except ValueError as exc:
        raise ValueError(f"{name} must be int, got {raw!r}") from exc


RELI_TESTS_ENABLED = _env_bool(
    "SFEPS_ENABLE_RELI_TESTS", _env_bool("SFEPS_ENABLE_PERF_TESTS", False)
)
RELI_TRACK_TOGGLE_COUNT = _env_int("SFEPS_RELI_TRACK_TOGGLE_COUNT", 20)
RELI_TRACK_HOLD_SECONDS = _env_float("SFEPS_RELI_TRACK_HOLD_SECONDS", 0.1)
RELI_TRACK_OBJECT_ID = (os.getenv("SFEPS_RELI_TRACK_OBJECT_ID") or "RELI-NF-01").strip()
STREAM_DURATION_SECONDS = _env_float(
    "SFEPS_RELI_STREAM_DURATION_SECONDS",
    _env_float("SFEPS_PERF_STREAM_DURATION_SECONDS", 3600.0),
)
STREAM_POLL_INTERVAL_SECONDS = _env_float(
    "SFEPS_RELI_STREAM_POLL_INTERVAL_SECONDS",
    _env_float("SFEPS_PERF_STREAM_POLL_INTERVAL_SECONDS", 1.0),
)
STREAM_RECOVERY_TIMEOUT_SECONDS = _env_float(
    "SFEPS_RELI_STREAM_RECOVERY_TIMEOUT_SECONDS",
    _env_float("SFEPS_PERF_STREAM_RECOVERY_TIMEOUT_SECONDS", 10.0),
)

pytestmark = pytest.mark.skipif(
    not RELI_TESTS_ENABLED,
    reason=(
        "Reliability tests are disabled by default. "
        "Set SFEPS_ENABLE_RELI_TESTS=1 (or SFEPS_ENABLE_PERF_TESTS=1)."
    ),
)


@pytest.fixture(scope="module")
def auth_endpoint() -> tuple[str, int]:
    try:
        with socket.create_connection((AUTH_HOST, AUTH_PORT), timeout=2.0):
            pass
    except OSError as exc:
        pytest.skip(f"Real auth server unavailable at {AUTH_HOST}:{AUTH_PORT} ({exc})")

    return AUTH_HOST, AUTH_PORT


def _is_listening(host: str, port: int, timeout: float = 0.5) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


@pytest.fixture(scope="module")
def reliability_endpoints(auth_endpoint) -> tuple[str, int, int]:
    host, _ = auth_endpoint
    if not _is_listening(host, ALERT_PORT, timeout=1.0):
        pytest.skip(f"Alert server unavailable at {host}:{ALERT_PORT}")
    if not _is_listening(host, POSITION_PORT, timeout=1.0):
        pytest.skip(f"Position server unavailable at {host}:{POSITION_PORT}")
    return host, ALERT_PORT, POSITION_PORT


def _send_auth_request(
    host: str, port: int, user_id: str, password: str, timeout: float = 3.0
) -> str:
    payload = f"{user_id}:{password}".encode("utf-8")
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(payload)
        response = sock.recv(16)
    return response.decode("utf-8", errors="replace").strip()


class _LineReader:
    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.buf = b""

    def recv_until(self, predicate, timeout: float) -> Optional[str]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while b"\n" in self.buf:
                raw_line, self.buf = self.buf.split(b"\n", 1)
                line = raw_line.decode("utf-8", errors="replace").strip()
                if line and predicate(line):
                    return line
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                return None
            self.buf += chunk
        return None


def _wait_for_socket_closed(sock: socket.socket, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            data = sock.recv(64)
        except socket.timeout:
            continue
        except OSError:
            return True
        if data == b"":
            return True
        return False
    return False


def _drain_alert_lines(reader: _LineReader, timeout: float = 0.05, max_lines: int = 32) -> list[str]:
    lines: list[str] = []
    deadline = time.monotonic() + timeout
    while len(lines) < max_lines and time.monotonic() < deadline:
        line = reader.recv_until(lambda _: True, timeout=min(0.02, deadline - time.monotonic()))
        if line is None:
            break
        lines.append(line)
    return lines


def _read_log_delta(start_offset: Optional[int]) -> str:
    if start_offset is None or not SERVER_LOG_PATH.is_file():
        return ""
    with SERVER_LOG_PATH.open("r", encoding="utf-8", errors="replace") as fp:
        fp.seek(start_offset)
        return fp.read()


def _count_track_log_lines(log_blob: str, token: str, object_id: str) -> int:
    count = 0
    for line in log_blob.splitlines():
        if token in line and f"object_id={object_id}" in line:
            count += 1
    return count


@dataclass(frozen=True)
class RtspEndpoint:
    raw_url: str
    host: str
    port: int


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
        "User-Agent": "SFEPS-RELI-PyTest",
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


@pytest.fixture(scope="module")
def stream_endpoint() -> RtspEndpoint:
    return _parse_rtsp_endpoint(_resolve_rtsp_url())


def _wait_until(predicate, timeout: float, interval: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return False


def test_tc_nf_reli_01(reliability_endpoints):
    if RELI_TRACK_TOGGLE_COUNT <= 0:
        pytest.skip("SFEPS_RELI_TRACK_TOGGLE_COUNT must be > 0")
    if RELI_TRACK_HOLD_SECONDS < 0:
        pytest.skip("SFEPS_RELI_TRACK_HOLD_SECONDS must be >= 0")
    if not RELI_TRACK_OBJECT_ID:
        pytest.skip("SFEPS_RELI_TRACK_OBJECT_ID must not be empty")

    host, alert_port, position_port = reliability_endpoints
    log_start_offset = SERVER_LOG_PATH.stat().st_size if SERVER_LOG_PATH.is_file() else None

    with socket.create_connection((host, alert_port), timeout=2.0) as alert_sock:
        alert_sock.settimeout(0.2)
        alert_reader = _LineReader(alert_sock)

        assert _send_auth_request(host, AUTH_PORT, "admin", "1111") == "PASS"
        login_ack = alert_reader.recv_until(
            lambda line: line.startswith("TEST|LOGIN_OK|"), timeout=3.0
        )
        assert login_ack is not None, "로그인 후 alert 채널에서 TEST|LOGIN_OK 수신 실패"

        force_logout_lines: list[str] = []
        with socket.create_connection((host, position_port), timeout=2.0) as pos_sock:
            pos_sock.settimeout(0.2)

            for idx in range(RELI_TRACK_TOGGLE_COUNT):
                pos_sock.sendall(f"SUB_POS|{RELI_TRACK_OBJECT_ID}\n".encode("utf-8"))
                if RELI_TRACK_HOLD_SECONDS > 0:
                    time.sleep(RELI_TRACK_HOLD_SECONDS)
                assert not _wait_for_socket_closed(pos_sock, timeout=0.15), (
                    "TC-NF-RELI-01 failed: Position connection closed after SUB_POS. "
                    f"iteration={idx + 1}/{RELI_TRACK_TOGGLE_COUNT}"
                )

                pos_sock.sendall(f"UNSUB_POS|{RELI_TRACK_OBJECT_ID}\n".encode("utf-8"))
                if RELI_TRACK_HOLD_SECONDS > 0:
                    time.sleep(RELI_TRACK_HOLD_SECONDS)
                assert not _wait_for_socket_closed(pos_sock, timeout=0.15), (
                    "TC-NF-RELI-01 failed: Position connection closed after UNSUB_POS. "
                    f"iteration={idx + 1}/{RELI_TRACK_TOGGLE_COUNT}"
                )

                for line in _drain_alert_lines(alert_reader, timeout=0.05):
                    if line.startswith("AUTH|FORCE_LOGOUT|"):
                        force_logout_lines.append(line)

        assert not force_logout_lines, (
            "TC-NF-RELI-01 failed: tracking 반복 중 FORCE_LOGOUT 이벤트 발생.\n"
            + "\n".join(force_logout_lines[:5])
        )

        assert _send_auth_request(host, AUTH_PORT, "admin", "1111") == "PASS"
        relogin_ack = alert_reader.recv_until(
            lambda line: line.startswith("TEST|LOGIN_OK|"), timeout=3.0
        )
        assert relogin_ack is not None, "반복 종료 후 재인증 TEST|LOGIN_OK 수신 실패"

        with socket.create_connection((host, position_port), timeout=2.0) as pos_sock2:
            pos_sock2.settimeout(0.2)
            pos_sock2.sendall(f"SUB_POS|{RELI_TRACK_OBJECT_ID}\n".encode("utf-8"))
            assert not _wait_for_socket_closed(pos_sock2, timeout=0.5), (
                "TC-NF-RELI-01 failed: 반복 종료 후 Position 연결 복구 실패"
            )
            pos_sock2.sendall(f"UNSUB_POS|{RELI_TRACK_OBJECT_ID}\n".encode("utf-8"))

    log_delta = _read_log_delta(log_start_offset)
    if log_delta:
        sub_count = _count_track_log_lines(log_delta, "[Position] SUB_POS 수신:", RELI_TRACK_OBJECT_ID)
        unsub_count = _count_track_log_lines(
            log_delta, "[Position] UNSUB_POS 수신:", RELI_TRACK_OBJECT_ID
        )
        expected_min = RELI_TRACK_TOGGLE_COUNT
        assert sub_count >= expected_min, (
            "TC-NF-RELI-01 failed: 서버 로그 SUB_POS 수가 부족합니다. "
            f"expected>={expected_min}, actual={sub_count}, object_id={RELI_TRACK_OBJECT_ID}"
        )
        assert unsub_count >= expected_min, (
            "TC-NF-RELI-01 failed: 서버 로그 UNSUB_POS 수가 부족합니다. "
            f"expected>={expected_min}, actual={unsub_count}, object_id={RELI_TRACK_OBJECT_ID}"
        )


def test_tc_nf_reli_02(stream_endpoint: RtspEndpoint, auth_endpoint):
    if STREAM_DURATION_SECONDS <= 0:
        pytest.skip("SFEPS_RELI_STREAM_DURATION_SECONDS must be > 0")
    if STREAM_POLL_INTERVAL_SECONDS <= 0:
        pytest.skip("SFEPS_RELI_STREAM_POLL_INTERVAL_SECONDS must be > 0")
    if STREAM_RECOVERY_TIMEOUT_SECONDS <= 0:
        pytest.skip("SFEPS_RELI_STREAM_RECOVERY_TIMEOUT_SECONDS must be > 0")

    if not _is_stream_ready(stream_endpoint, timeout=3.0):
        pytest.skip(
            "TC-NF-RELI-02 pre-condition 미충족: "
            f"DESCRIBE 200 + video 트랙 확인 실패 ({stream_endpoint.raw_url})"
        )

    deadline = time.monotonic() + STREAM_DURATION_SECONDS
    outage_count = 0
    max_outage_seconds = 0.0
    total_outage_seconds = 0.0

    while time.monotonic() < deadline:
        if _is_stream_ready(stream_endpoint, timeout=3.0):
            time.sleep(STREAM_POLL_INTERVAL_SECONDS)
            continue

        outage_count += 1
        outage_start = time.monotonic()
        recovered = _wait_until(
            lambda: _is_stream_ready(stream_endpoint, timeout=3.0),
            STREAM_RECOVERY_TIMEOUT_SECONDS,
            min(1.0, STREAM_POLL_INTERVAL_SECONDS),
        )
        outage_duration = time.monotonic() - outage_start
        max_outage_seconds = max(max_outage_seconds, outage_duration)
        total_outage_seconds += outage_duration

        assert recovered, (
            "TC-NF-RELI-02 failed: stream outage was not recovered in allowed timeout. "
            f"timeout={STREAM_RECOVERY_TIMEOUT_SECONDS:.3f}s, "
            f"outage_count={outage_count}, endpoint={stream_endpoint.raw_url}"
        )
