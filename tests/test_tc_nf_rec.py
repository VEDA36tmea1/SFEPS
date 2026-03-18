import os
import socket
import time
from typing import Callable, Optional

import pytest


AUTH_HOST = "127.0.0.1"
AUTH_PORT = 5555
ALERT_PORT = 5557
POSITION_PORT = 5558


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

    def recv_until(
        self, predicate: Callable[[str], bool], timeout: float
    ) -> Optional[str]:
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


def _deauth_wait_seconds() -> float:
    raw = os.getenv("SFEPS_AUTH_DEAUTH_GRACE_MS", "3000").strip()
    try:
        grace_ms = max(0, int(raw))
    except ValueError:
        grace_ms = 3000
    return (grace_ms / 1000.0) + 1.5


@pytest.fixture(scope="module")
def recovery_endpoints() -> tuple[str, int, int]:
    if not _is_listening(AUTH_HOST, AUTH_PORT, timeout=1.0):
        pytest.skip(f"Auth server unavailable at {AUTH_HOST}:{AUTH_PORT}")
    if not _is_listening(AUTH_HOST, ALERT_PORT, timeout=1.0):
        pytest.skip(f"Alert server unavailable at {AUTH_HOST}:{ALERT_PORT}")
    return AUTH_HOST, ALERT_PORT, POSITION_PORT


def test_tc_nf_rec_01(recovery_endpoints):
    host, alert_port, position_port = recovery_endpoints

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
            pos_sock.sendall(b"SUB_POS|REC-NF-01\n")
            closed_early = _wait_for_socket_closed(pos_sock, timeout=0.7)
            assert not closed_early, "로그인 직후 Position 연결이 비정상적으로 종료됨"

        time.sleep(_deauth_wait_seconds())

        with socket.create_connection((host, position_port), timeout=2.0) as pos_sock2:
            pos_sock2.settimeout(0.2)
            try:
                pos_sock2.sendall(b"SUB_POS|REC-NF-01\n")
            except OSError:
                pass
            closed_for_unauth = _wait_for_socket_closed(pos_sock2, timeout=2.0)

        assert closed_for_unauth, "재인증 없이 Position 재접속이 허용됨(예상: 거절/연결종료)"

        force_logout = alert_reader.recv_until(
            lambda line: line.startswith("AUTH|FORCE_LOGOUT|"), timeout=3.0
        )
        assert force_logout is not None, "unauthenticated 재접속 후 FORCE_LOGOUT 이벤트 미수신"
        assert "REASON=POSITION_UNAUTHENTICATED" in force_logout

        assert _send_auth_request(host, AUTH_PORT, "admin", "1111") == "PASS"
        login_ack2 = alert_reader.recv_until(
            lambda line: line.startswith("TEST|LOGIN_OK|"), timeout=3.0
        )
        assert login_ack2 is not None, "재로그인 후 TEST|LOGIN_OK 수신 실패"

        with socket.create_connection((host, position_port), timeout=2.0) as pos_sock3:
            pos_sock3.settimeout(0.2)
            pos_sock3.sendall(b"SUB_POS|REC-NF-01\n")
            closed_after_reauth = _wait_for_socket_closed(pos_sock3, timeout=0.7)
            assert not closed_after_reauth, "재로그인 후 Position 연결이 유지되지 않음"
