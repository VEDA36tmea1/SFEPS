import os
import socket
from pathlib import Path

import pytest


ROOT_DIR = Path(__file__).resolve().parents[1]
AUTH_MANAGER_CPP = ROOT_DIR / "client" / "src" / "authmanager.cpp"
CLIENT_MAIN_CPP = ROOT_DIR / "client" / "src" / "main.cpp"
LOGIN_VIEW_QML = ROOT_DIR / "client" / "src" / "views" / "LoginView.qml"


@pytest.fixture(scope="module")
def auth_endpoint():
    host = "127.0.0.1"
    port = 5555

    try:
        with socket.create_connection((host, port), timeout=2.0):
            pass
    except OSError as exc:
        pytest.skip(f"Auth server unavailable at {host}:{port} ({exc})")

    return host, port


def send_auth_request(host: str, port: int, user_id: str, password: str, timeout: float = 3.0) -> str:
    payload = f"{user_id}:{password}".encode("utf-8")
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(payload)
        response = sock.recv(16)
    return response.decode("utf-8").strip()


def test_tc_func_login_01_valid_id_pw_login_success(auth_endpoint):
    host, port = auth_endpoint
    assert send_auth_request(host, port, "admin", "1111") == "PASS"


def test_tc_func_login_02_non_existing_id_login_fail(auth_endpoint):
    host, port = auth_endpoint
    assert send_auth_request(host, port, "no_user_999", "1111") == "FAIL"


def test_tc_func_login_03_wrong_password_login_fail(auth_endpoint):
    host, port = auth_endpoint
    assert send_auth_request(host, port, "admin", "WrongPW!") == "FAIL"


@pytest.mark.parametrize(
    "user_id,password",
    [
        ("", "1111"),
        ("admin", ""),
        ("", ""),
    ],
)
def test_tc_func_login_04_blank_input_login_fail(user_id, password, auth_endpoint):
    host, port = auth_endpoint
    assert send_auth_request(host, port, user_id, password) == "FAIL"


def test_tc_func_login_04_client_blocks_empty_input_before_server_call():
    source = AUTH_MANAGER_CPP.read_text(encoding="utf-8")

    guard_snippet = "if (userId.isEmpty() || pw.trimmed().isEmpty())"
    message_snippet = 'emit loginFailed("ID와 PW를 모두 입력하세요")'
    connect_snippet = "socket->connectToHost(host, static_cast<quint16>(port));"

    assert guard_snippet in source
    assert message_snippet in source
    assert "return;" in source
    assert source.find(guard_snippet) < source.find(connect_snippet)
