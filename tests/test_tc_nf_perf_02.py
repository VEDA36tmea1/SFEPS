import os
import socket
import time
from pathlib import Path

import pytest


def _env_int(name: str, default: int) -> int:
    value = os.getenv(name)
    if value is None:
        return default
    return int(value)


def _env_float(name: str, default: float) -> float:
    value = os.getenv(name)
    if value is None:
        return default
    return float(value)


def _recv_lines(sock: socket.socket, deadline: float) -> list[str]:
    buffer = b""
    lines: list[str] = []
    while time.monotonic() < deadline:
        timeout_left = max(0.05, min(1.0, deadline - time.monotonic()))
        sock.settimeout(timeout_left)
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue

        if not chunk:
            break

        buffer += chunk
        while b"\n" in buffer:
            raw, buffer = buffer.split(b"\n", 1)
            text = raw.decode("utf-8", errors="replace").strip()
            if text:
                lines.append(text)

    remain = buffer.decode("utf-8", errors="replace").strip()
    if remain:
        lines.append(remain)
    return lines


def _build_rc522_line(uid_hex: str, text: str, timestamp: int, device_id: int) -> str:
    return (
        f'{{"device_id":{device_id},"id":"{uid_hex}","text":"{text}","timestamp":{timestamp}}}\n'
    )


def _inject_via_uds_daemon_emulator(
    socket_path: str,
    target_count: int,
    card_text: str,
    uid_seed: int,
    send_interval_sec: float,
    accept_timeout_sec: float,
    device_id: int,
) -> tuple[int, str, str]:
    if os.name == "nt":
        return 1, "", "uds mode requires non-Windows environment (AF_UNIX)"

    uds = Path(socket_path)
    if uds.exists():
        try:
            uds.unlink()
        except OSError as error:
            return 1, "", f"failed to remove existing socket path: {socket_path}, error={error}"

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        server.bind(socket_path)
        server.listen(1)
        server.settimeout(accept_timeout_sec)

        conn, _ = server.accept()
        with conn:
            for index in range(target_count):
                uid = (uid_seed + index) & 0xFFFFFFFF
                uid_hex = f"{uid:08X}"
                line = _build_rc522_line(
                    uid_hex=uid_hex,
                    text=card_text,
                    timestamp=int(time.time()),
                    device_id=device_id,
                )
                conn.sendall(line.encode("utf-8"))
                if send_interval_sec > 0:
                    time.sleep(send_interval_sec)

        return 0, f"sent {target_count} ndjson lines via uds {socket_path}", ""
    except Exception as error:
        return 1, "", str(error)
    finally:
        try:
            server.close()
        except OSError:
            pass
        if uds.exists():
            try:
                uds.unlink()
            except OSError:
                pass


def test_tc_nf_perf_02_suspicious_events_20_within_1min():
    alert_host = os.getenv("SFEPS_PERF_ALERT_HOST", os.getenv("FRAUD_SERVER_HOST", "192.168.0.92"))
    alert_port = _env_int("SFEPS_PERF_ALERT_PORT", 5557)

    target_count = _env_int("SFEPS_PERF_TARGET_COUNT", 20)
    window_sec = _env_float("SFEPS_PERF_WINDOW_SEC", 60.0)
    allowed_missing = _env_int("SFEPS_PERF_ALLOWED_MISSING", 0)
    allowed_duplicate = _env_int("SFEPS_PERF_ALLOWED_DUPLICATE", 0)
    message_prefix = os.getenv("SFEPS_PERF_MESSAGE_PREFIX", "FRAUD|")
    rfid_socket_path = os.getenv("SFEPS_PERF_RFID_SOCKET_PATH", "/tmp/rc522_events.sock")
    rfid_card_text = os.getenv("SFEPS_PERF_RFID_TEXT", "Invalid")
    rfid_uid_seed = _env_int("SFEPS_PERF_RFID_UID_SEED", 0xA0000000)
    rfid_device_id = _env_int("SFEPS_PERF_RFID_DEVICE_ID", 1)
    rfid_send_interval_sec = _env_float("SFEPS_PERF_RFID_SEND_INTERVAL_SEC", 0.05)
    rfid_accept_timeout_sec = _env_float("SFEPS_PERF_RFID_ACCEPT_TIMEOUT_SEC", 10.0)

    if os.name == "nt":
        pytest.skip("TC-NF-PERF-02 UDS injector mode requires Linux/Unix (AF_UNIX)")

    start_monotonic = time.monotonic()
    deadline = start_monotonic + window_sec

    with socket.create_connection((alert_host, alert_port), timeout=5.0) as sock:
        return_code, injector_stdout, injector_stderr = _inject_via_uds_daemon_emulator(
            socket_path=rfid_socket_path,
            target_count=target_count,
            card_text=rfid_card_text,
            uid_seed=rfid_uid_seed,
            send_interval_sec=rfid_send_interval_sec,
            accept_timeout_sec=rfid_accept_timeout_sec,
            device_id=rfid_device_id,
        )
        assert return_code == 0, (
            "Injector failed. "
            f"injector=uds:{rfid_socket_path}, rc={return_code}, stderr={injector_stderr[:300]}, stdout={injector_stdout[:300]}"
        )

        messages = _recv_lines(sock, deadline=deadline)

    elapsed_sec = time.monotonic() - start_monotonic

    matched = [message for message in messages if message.startswith(message_prefix)]
    displayed_count = len(matched)
    unique_count = len(set(matched))
    duplicate_by_count = max(0, displayed_count - unique_count)
    missing_by_count = max(0, target_count - unique_count)

    assert elapsed_sec <= window_sec, (
        f"Processing took too long: elapsed={elapsed_sec:.2f}s, limit={window_sec:.2f}s"
    )
    assert unique_count >= target_count, (
        f"Too few events delivered to alert channel: unique={unique_count}, total={displayed_count}, "
        f"target={target_count}, host={alert_host}, port={alert_port}, prefix={message_prefix}"
    )

    assert missing_by_count <= allowed_missing, (
        f"Missing events exceed tolerance: missing={missing_by_count}, allowed={allowed_missing}"
    )
    assert duplicate_by_count <= allowed_duplicate, (
        f"Duplicate events exceed tolerance: duplicate={duplicate_by_count}, allowed={allowed_duplicate}"
    )