import os
import socket
import subprocess
import time
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
SERVER_DIR = REPO_ROOT / "server"
AUTH_HOST = "127.0.0.1"
AUTH_PORT = 5555
SERVER_START_TIMEOUT_SEC = 12.0
SERVER_LOG_PATH = Path(__file__).resolve().parent / "real_server.log"


def _find_server_binary():
    candidates = [
        SERVER_DIR / "build" / "smart_server.bin",
        SERVER_DIR / "build" / "smart_server",
        SERVER_DIR / "build" / "smart_server.exe",
    ]
    for path in candidates:
        if path.is_file():
            return path
    return None


def _is_listening(host: str, port: int, timeout: float = 0.2) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def _load_env_file(path: Path) -> dict[str, str]:
    env_map: dict[str, str] = {}
    if not path.is_file():
        return env_map

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[len("export ") :].strip()
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        if key:
            env_map[key] = value
    return env_map


def _prepare_real_server_env() -> tuple[dict[str, str], list[str]]:
    env = os.environ.copy()

    # Prefer explicit process env; use file values only for missing keys.
    for env_file in (SERVER_DIR / ".env", SERVER_DIR / ".env.local"):
        for key, value in _load_env_file(env_file).items():
            env.setdefault(key, value)

    # Test-safe defaults.
    env.setdefault("SFEPS_DB_HOST", "localhost")
    env.setdefault("SFEPS_AUTH_ALLOW_IPS", "127.0.0.1")
    env.setdefault("SFEPS_AUDIO_ALLOW_IPS", "127.0.0.1")
    env.setdefault("SFEPS_ALERT_ALLOW_IPS", "127.0.0.1")

    # Force plaintext auth for local login tests.
    env["SFEPS_APP_TLS_ENABLE"] = "0"
    env["SFEPS_APP_PLAINTEXT_ENABLE"] = "1"
    env["SFEPS_APP_BIND_IP"] = "127.0.0.1"

    required = [
        "SFEPS_DB_USER",
        "SFEPS_DB_PASS",
        "SFEPS_DB_NAME_ANALYTICS",
    ]
    missing = [key for key in required if not env.get(key)]
    return env, missing


def _read_log_tail(path: Path, max_lines: int = 20) -> str:
    if not path.exists():
        return ""
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    return "\n".join(lines[-max_lines:])


@pytest.fixture(scope="session", autouse=True)
def real_auth_server():
    """Ensure tests use the real SFEPS server auth listener on 127.0.0.1:5555."""
    if _is_listening(AUTH_HOST, AUTH_PORT):
        yield
        return

    server_bin = _find_server_binary()
    if server_bin is None:
        pytest.skip(
            "Real SFEPS server binary not found. Build server first "
            "(e.g. cmake -S server -B server/build && cmake --build server/build)."
        )

    env, missing = _prepare_real_server_env()
    if missing:
        pytest.skip(
            "Real SFEPS server env is incomplete: missing "
            + ", ".join(missing)
            + ". Set them in shell or server/.env.local."
        )

    log_fp = SERVER_LOG_PATH.open("w", encoding="utf-8")
    proc = subprocess.Popen(
        [str(server_bin)],
        cwd=str(SERVER_DIR),
        env=env,
        stdout=log_fp,
        stderr=subprocess.STDOUT,
    )

    try:
        deadline = time.monotonic() + SERVER_START_TIMEOUT_SEC
        while time.monotonic() < deadline:
            if _is_listening(AUTH_HOST, AUTH_PORT):
                break
            if proc.poll() is not None:
                break
            time.sleep(0.1)

        if not _is_listening(AUTH_HOST, AUTH_PORT):
            exit_code = proc.poll()
            tail = _read_log_tail(SERVER_LOG_PATH)
            pytest.skip(
                "Failed to start real SFEPS auth server on 127.0.0.1:5555 "
                f"(exit_code={exit_code}). Check tests/real_server.log.\n{tail}"
            )

        yield
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        log_fp.close()
