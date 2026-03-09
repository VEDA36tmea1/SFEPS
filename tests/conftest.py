import socket
import threading
import time
import socketserver
import sys
import subprocess
import os
from pathlib import Path

import pytest

# Ensure tests/ is on sys.path so we can import mock_auth_server when running pytest
tests_dir = Path(__file__).resolve().parent
if str(tests_dir) not in sys.path:
    sys.path.insert(0, str(tests_dir))

from mock_auth_server import AuthHandler, HOST, PORT


class ThreadedTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True


def _find_server_binary():
    repo_root = Path(__file__).resolve().parents[1]
    candidates = [
        repo_root / 'server' / 'build' / 'smart_server.bin',
        repo_root / 'server' / 'build' / 'smart_server',
        repo_root / 'server' / 'build' / 'smart_server.exe',
    ]
    for p in candidates:
        if p.exists() and p.is_file():
            return str(p)
    return None


@pytest.fixture(scope="session", autouse=True)
def mock_auth_server():
    """Start an auth server for the test session.

    Preference order:
    1. If a built `server` binary exists and required environment variables
       for DB/connectivity are already provided, launch the real server.
    2. Otherwise, fall back to the lightweight `tests/mock_auth_server.py`.

    The fixture yields when the auth port is accepting connections and
    ensures the started process/server is cleaned up at session end.
    """
    host, port = HOST, PORT

    # Try to locate a built server binary and required env vars.
    bin_path = _find_server_binary()
    server_proc = None

    can_run_real = False
    if bin_path:
        required_envs = [
            'SFEPS_DB_USER', 'SFEPS_DB_PASS', 'SFEPS_DB_NAME_ANALYTICS',
            'SFEPS_AUTH_ALLOW_IPS', 'SFEPS_AUDIO_ALLOW_IPS', 'SFEPS_ALERT_ALLOW_IPS',
        ]
        missing = [v for v in required_envs if not os.environ.get(v)]
        if not missing:
            can_run_real = True
        else:
            # If any allowlist is missing, try to set them to localhost-only
            # so the server's fail-closed checks pass when a DB is present.
            # Only set if DB envs are present too.
            db_envs = ['SFEPS_DB_USER', 'SFEPS_DB_PASS', 'SFEPS_DB_NAME_ANALYTICS']
            if all(os.environ.get(v) for v in db_envs):
                os.environ.setdefault('SFEPS_AUTH_ALLOW_IPS', '127.0.0.1')
                os.environ.setdefault('SFEPS_AUDIO_ALLOW_IPS', '127.0.0.1')
                os.environ.setdefault('SFEPS_ALERT_ALLOW_IPS', '127.0.0.1')
                can_run_real = True

    if bin_path and can_run_real:
        try:
            env = os.environ.copy()
            # Ensure plaintext app mode for test compatibility
            env.setdefault('SFEPS_APP_TLS_ENABLE', '0')
            env.setdefault('SFEPS_APP_PLAINTEXT_ENABLE', '1')

            server_proc = subprocess.Popen([bin_path], env=env)

            # wait for server to accept connections
            for _ in range(80):
                try:
                    with socket.create_connection((host, port), timeout=0.2):
                        break
                except Exception:
                    time.sleep(0.05)
            else:
                # server didn't come up; kill and fall back
                server_proc.terminate()
                server_proc.wait(timeout=1)
                server_proc = None
        except Exception:
            server_proc = None

    if server_proc is None:
        # Fall back to lightweight mock server used previously.
        server = ThreadedTCPServer((host, port), AuthHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()

        # wait for mock server to become available
        for _ in range(40):
            try:
                with socket.create_connection((host, port), timeout=0.2):
                    break
            except Exception:
                time.sleep(0.05)

        yield

        server.shutdown()
        server.server_close()
        thread.join(timeout=1)
        return

    # If we started the real server process, yield and ensure cleanup.
    try:
        yield
    finally:
        try:
            server_proc.terminate()
            server_proc.wait(timeout=5)
        except Exception:
            try:
                server_proc.kill()
            except Exception:
                pass
