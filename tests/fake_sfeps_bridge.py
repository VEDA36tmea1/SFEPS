#!/usr/bin/env python3
import os
import socket
import sys
import time


def _env(name, default):
    return os.getenv(name, default)


def main():
    alert_host = _env("SFEPS_PERF_ALERT_HOST", "127.0.0.1")
    alert_port = int(_env("SFEPS_PERF_ALERT_PORT", "5557"))
    rfid_socket_path = _env("SFEPS_PERF_RFID_SOCKET_PATH", "/tmp/rc522_events.sock")
    accept_timeout = float(_env("SFEPS_PERF_RFID_ACCEPT_TIMEOUT_SEC", "10"))

    # Start alert TCP server to accept the test client
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", alert_port))
    server.listen(1)
    print(f"fake_sfeps_bridge: alert server listening on 0.0.0.0:{alert_port}", flush=True)

    # Wait for the test to connect
    conn, addr = server.accept()
    print(f"fake_sfeps_bridge: accepted connection from {addr}", flush=True)

    # Connect to the UDS injector (the test's injector will bind and accept)
    try:
        uds = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        # wait for socket path to appear (respect accept timeout + small buffer)
        wait_seconds = max(accept_timeout + 2.0, 2.0)
        waited = 0.0
        poll = 0.1
        print(f"fake_sfeps_bridge: waiting up to {wait_seconds}s for UDS {rfid_socket_path}", flush=True)
        while waited < wait_seconds:
            if os.path.exists(rfid_socket_path):
                break
            time.sleep(poll)
            waited += poll
        if not os.path.exists(rfid_socket_path):
            raise TimeoutError(f"UDS path did not appear within {wait_seconds}s: {rfid_socket_path}")
        uds.connect(rfid_socket_path)
    except Exception as exc:
        print(f"fake_sfeps_bridge: failed to connect to uds {rfid_socket_path}: {exc}", flush=True)
        conn.close()
        server.close()
        sys.exit(1)

    try:
        # Relay every NDJSON line read from the uds as FRAUD|<json>\n to the connected client
        buffer = b""
        while True:
            chunk = uds.recv(4096)
            if not chunk:
                break
            buffer += chunk
            while b"\n" in buffer:
                raw, buffer = buffer.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                message = f"FRAUD|{line}\n"
                try:
                    conn.sendall(message.encode("utf-8"))
                except BrokenPipeError:
                    print("fake_sfeps_bridge: client closed connection", flush=True)
                    break
    finally:
        try:
            uds.close()
        except Exception:
            pass
        try:
            conn.close()
        except Exception:
            pass
        try:
            server.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
