import os
import socket
import time
from pathlib import Path
from typing import Optional

import pytest


AUTH_HOST = "127.0.0.1"
AUTH_PORT = 5555
ALERT_PORT = 5557
POSITION_PORT = 5558
SERVER_LOG_PATH = Path(__file__).resolve().parent / "real_server.log"


def _env_float(name: str, default: float) -> float:
    raw = os.getenv(name)
    if raw is None:
        return default
    try:
        return float(raw)
    except ValueError as exc:
        raise ValueError(f"{name} must be float, got {raw!r}") from exc


TRACK_CMD_TIMEOUT_SECONDS = _env_float("SFEPS_TRACK_CMD_TIMEOUT_SECONDS", 3.0)
TRACK_OBJECT_ID_PREFIX = (os.getenv("SFEPS_TRACK_OBJECT_ID") or "FUNC-TRACK-01").strip()


def _is_listening(host: str, port: int, timeout: float = 0.5) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


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


def _drain_lines(reader: _LineReader, timeout: float = 0.15, max_lines: int = 32) -> list[str]:
    lines: list[str] = []
    deadline = time.monotonic() + timeout
    while len(lines) < max_lines and time.monotonic() < deadline:
        remaining = max(0.0, min(0.03, deadline - time.monotonic()))
        if remaining <= 0:
            break
        line = reader.recv_until(lambda _: True, timeout=remaining)
        if line is None:
            break
        lines.append(line)
    return lines


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


@pytest.fixture(scope="module")
def auth_endpoint() -> tuple[str, int]:
    try:
        with socket.create_connection((AUTH_HOST, AUTH_PORT), timeout=2.0):
            pass
    except OSError as exc:
        pytest.skip(f"Real auth server unavailable at {AUTH_HOST}:{AUTH_PORT} ({exc})")
    return AUTH_HOST, AUTH_PORT


@pytest.fixture(scope="module")
def track_endpoints(auth_endpoint) -> tuple[str, int, int]:
    host, _ = auth_endpoint
    if not _is_listening(host, ALERT_PORT, timeout=1.0):
        pytest.skip(f"Alert server unavailable at {host}:{ALERT_PORT}")
    if not _is_listening(host, POSITION_PORT, timeout=1.0):
        pytest.skip(f"Position server unavailable at {host}:{POSITION_PORT}")
    return host, ALERT_PORT, POSITION_PORT


def test_tc_func_track_01(track_endpoints):
    if TRACK_CMD_TIMEOUT_SECONDS <= 0:
        pytest.skip("SFEPS_TRACK_CMD_TIMEOUT_SECONDS must be > 0")
    if not TRACK_OBJECT_ID_PREFIX:
        pytest.skip("SFEPS_TRACK_OBJECT_ID must not be empty")

    host, alert_port, position_port = track_endpoints
    object_id = f"{TRACK_OBJECT_ID_PREFIX}-{int(time.time() * 1000)}"
    log_start_offset = SERVER_LOG_PATH.stat().st_size if SERVER_LOG_PATH.is_file() else None

    with socket.create_connection((host, alert_port), timeout=2.0) as alert_sock:
        alert_sock.settimeout(0.2)
        alert_reader = _LineReader(alert_sock)

        assert _send_auth_request(host, AUTH_PORT, "admin", "1111") == "PASS"
        login_ack = alert_reader.recv_until(
            lambda line: line.startswith("TEST|LOGIN_OK|"), timeout=3.0
        )
        assert login_ack is not None, "로그인 후 alert 채널에서 TEST|LOGIN_OK 수신 실패"

        with socket.create_connection((host, position_port), timeout=2.0) as pos_sock:
            pos_sock.settimeout(0.2)

            pos_sock.sendall(f"SUB_POS|{object_id}\n".encode("utf-8"))
            assert not _wait_for_socket_closed(pos_sock, timeout=0.5), (
                "TC-FUNC-TRACK-01 failed: SUB_POS 후 Position 연결이 비정상 종료됨 "
                f"(object_id={object_id})"
            )

            pos_sock.sendall(f"UNSUB_POS|{object_id}\n".encode("utf-8"))
            assert not _wait_for_socket_closed(pos_sock, timeout=0.5), (
                "TC-FUNC-TRACK-01 failed: UNSUB_POS 후 Position 연결이 비정상 종료됨 "
                f"(object_id={object_id})"
            )

        force_logout_lines = [
            line
            for line in _drain_lines(alert_reader, timeout=0.2)
            if line.startswith("AUTH|FORCE_LOGOUT|")
        ]
        assert not force_logout_lines, (
            "TC-FUNC-TRACK-01 failed: Track/Untrack 처리 중 FORCE_LOGOUT 이벤트 발생.\n"
            + "\n".join(force_logout_lines[:5])
        )

    log_deadline = time.monotonic() + TRACK_CMD_TIMEOUT_SECONDS
    log_delta = ""
    while time.monotonic() < log_deadline:
        log_delta = _read_log_delta(log_start_offset)
        sub_count = _count_track_log_lines(log_delta, "[Position] SUB_POS received:", object_id)
        unsub_count = _count_track_log_lines(
            log_delta, "[Position] UNSUB_POS received:", object_id
        )
        if sub_count >= 1 and unsub_count >= 1:
            break
        time.sleep(0.05)

    if log_start_offset is not None:
        assert sub_count >= 1, (
            "TC-FUNC-TRACK-01 failed: 서버 SUB_POS 수신 로그 미확인 "
            f"(object_id={object_id})"
        )
        assert unsub_count >= 1, (
            "TC-FUNC-TRACK-01 failed: 서버 UNSUB_POS 수신 로그 미확인 "
            f"(object_id={object_id})"
        )
