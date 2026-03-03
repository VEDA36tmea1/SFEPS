import socket
import threading
import time
import socketserver
import sys
from pathlib import Path

import pytest

# Ensure tests/ is on sys.path so we can import mock_auth_server when running pytest
tests_dir = Path(__file__).resolve().parent
if str(tests_dir) not in sys.path:
    sys.path.insert(0, str(tests_dir))

from mock_auth_server import AuthHandler, HOST, PORT


class ThreadedTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True


@pytest.fixture(scope="session", autouse=True)
def mock_auth_server():
    """Start mock auth server on 127.0.0.1:5555 for the test session.

    This fixture is autouse so tests that expect the auth endpoint to be
    available can run without manual server startup. The server is shut down
    after the test session completes.
    """
    host, port = HOST, PORT

    # Always start the local mock server for tests.
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
