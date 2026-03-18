import json
import os
import socket
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Optional
from urllib.parse import urlparse

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
EVENT_DRIVER_SRC = REPO_ROOT / "tests" / "server_event_driver.cpp"
EVENT_DRIVER_BIN_DIR = REPO_ROOT / "tests" / ".bin"
EVENT_DRIVER_BIN = EVENT_DRIVER_BIN_DIR / "server_event_driver"
AUTH_HOST = "127.0.0.1"
AUTH_PORT = 5555


def _env_bool(name: str, default: bool) -> bool:
    raw = os.getenv(name)
    if raw is None:
        return default
    return raw.strip().lower() in ("1", "true", "yes", "on")


def _env_int(name: str, default: int) -> int:
    raw = os.getenv(name)
    if raw is None:
        return default
    try:
        return int(raw)
    except ValueError as exc:
        raise ValueError(f"{name} must be int, got {raw!r}") from exc


def _env_float(name: str, default: float) -> float:
    raw = os.getenv(name)
    if raw is None:
        return default
    try:
        return float(raw)
    except ValueError as exc:
        raise ValueError(f"{name} must be float, got {raw!r}") from exc


PERF_TESTS_ENABLED = _env_bool("SFEPS_ENABLE_PERF_TESTS", False)
EVENT_TARGET_COUNT = _env_int("SFEPS_PERF_EVENT_TARGET_COUNT", 50)
EVENT_WINDOW_SECONDS = _env_float("SFEPS_PERF_EVENT_WINDOW_SECONDS", 60.0)
EVENT_INTERVAL_SECONDS = _env_float("SFEPS_PERF_EVENT_INTERVAL_SECONDS", 0.0)
EVENT_MAX_MISSING = _env_int("SFEPS_PERF_EVENT_MAX_MISSING", 0)
EVENT_MAX_DUPLICATES = _env_int("SFEPS_PERF_EVENT_MAX_DUPLICATES", 0)
STREAM_DURATION_SECONDS = _env_float("SFEPS_PERF_STREAM_DURATION_SECONDS", 3600.0)
STREAM_POLL_INTERVAL_SECONDS = _env_float("SFEPS_PERF_STREAM_POLL_INTERVAL_SECONDS", 1.0)
STREAM_RECOVERY_TIMEOUT_SECONDS = _env_float(
    "SFEPS_PERF_STREAM_RECOVERY_TIMEOUT_SECONDS", 10.0
)

pytestmark = pytest.mark.skipif(
    not PERF_TESTS_ENABLED,
    reason=(
        "Performance tests are disabled by default. "
        "Set SFEPS_ENABLE_PERF_TESTS=1 for dedicated Jenkins performance runs."
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


def _compile_event_driver() -> None:
    EVENT_DRIVER_BIN_DIR.mkdir(parents=True, exist_ok=True)

    pkg_flags = (
        subprocess.check_output(["pkg-config", "--cflags", "--libs", "mariadb"], text=True)
        .strip()
        .split()
    )

    cmd = [
        "g++",
        "-std=c++17",
        "-O2",
        "-pthread",
        f"-I{REPO_ROOT / 'server' / 'include'}",
        f"-I{REPO_ROOT / 'Camera' / 'get_metadata' / 'inc'}",
        str(EVENT_DRIVER_SRC),
        str(REPO_ROOT / "server" / "src" / "analytics.cpp"),
        str(REPO_ROOT / "Camera" / "get_metadata" / "src" / "XMLParser.cpp"),
        str(REPO_ROOT / "server" / "src" / "rfid_monitor.cpp"),
        "-o",
        str(EVENT_DRIVER_BIN),
    ] + pkg_flags

    proc = subprocess.run(cmd, cwd=REPO_ROOT, text=True, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(
            "server_event_driver build failed.\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )


@pytest.fixture(scope="module")
def event_driver_bin():
    if not EVENT_DRIVER_SRC.is_file():
        pytest.skip(f"event driver source not found: {EVENT_DRIVER_SRC}")

    try:
        needs_build = (
            (not EVENT_DRIVER_BIN.exists())
            or EVENT_DRIVER_SRC.stat().st_mtime > EVENT_DRIVER_BIN.stat().st_mtime
        )
        if needs_build:
            _compile_event_driver()
    except FileNotFoundError as exc:
        pytest.skip(f"build tool missing: {exc}")
    except subprocess.CalledProcessError as exc:
        pytest.skip(f"pkg-config failed: {exc}")
    except RuntimeError as exc:
        pytest.skip(str(exc))

    return EVENT_DRIVER_BIN


def _run_driver_json(event_driver_bin: Path, *args: str) -> dict:
    proc = subprocess.run(
        [str(event_driver_bin), *args],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
    )
    if proc.returncode != 0:
        raise AssertionError(
            f"driver failed: cmd={[str(event_driver_bin), *args]}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )

    payload_line = None
    for line in reversed(proc.stdout.splitlines()):
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            payload_line = line
            break
    if payload_line is None:
        raise AssertionError(f"driver JSON output not found.\nstdout:\n{proc.stdout}")

    return json.loads(payload_line)


def test_tc_nf_perf_02(event_driver_bin, auth_endpoint):
    if EVENT_TARGET_COUNT <= 0:
        pytest.skip("SFEPS_PERF_EVENT_TARGET_COUNT must be > 0")
    if EVENT_WINDOW_SECONDS <= 0:
        pytest.skip("SFEPS_PERF_EVENT_WINDOW_SECONDS must be > 0")

    start = time.monotonic()
    success_count = 0
    returned_ids = set()
    duplicate_count = 0
    failures = []

    for idx in range(EVENT_TARGET_COUNT):
        object_id = f"PERF-OBJ-{idx:04d}"
        try:
            result = _run_driver_json(event_driver_bin, "run-case", "20", "youth", object_id)
        except Exception as exc:  # noqa: BLE001
            failures.append(f"{object_id}: {exc}")
            continue

        returned_object_id = str(result.get("object_id", ""))
        is_fraud = result.get("fraud") is True
        queue_size_ok = int(result.get("queue_size", 0)) >= 1

        if returned_object_id in returned_ids:
            duplicate_count += 1
        else:
            returned_ids.add(returned_object_id)

        if returned_object_id == object_id and is_fraud and queue_size_ok:
            success_count += 1
        else:
            failures.append(
                f"{object_id}: fraud={result.get('fraud')}, "
                f"queue_size={result.get('queue_size')}, returned_id={returned_object_id}"
            )

        if EVENT_INTERVAL_SECONDS > 0:
            time.sleep(EVENT_INTERVAL_SECONDS)

    elapsed = time.monotonic() - start
    missing_count = max(0, EVENT_TARGET_COUNT - len(returned_ids))
    failure_count = len(failures)

    assert elapsed <= EVENT_WINDOW_SECONDS, (
        "TC-NF-PERF-02 failed: elapsed time exceeded window. "
        f"elapsed={elapsed:.3f}s, window={EVENT_WINDOW_SECONDS:.3f}s, "
        f"target={EVENT_TARGET_COUNT}, success={success_count}, failures={failure_count}"
    )
    assert success_count >= EVENT_TARGET_COUNT - EVENT_MAX_MISSING, (
        "TC-NF-PERF-02 failed: processed success count below threshold. "
        f"success={success_count}, target={EVENT_TARGET_COUNT}, "
        f"allowed_missing={EVENT_MAX_MISSING}, failures={failure_count}"
    )
    assert missing_count <= EVENT_MAX_MISSING, (
        "TC-NF-PERF-02 failed: missing count exceeded threshold. "
        f"missing={missing_count}, allowed_missing={EVENT_MAX_MISSING}, "
        f"unique_returned={len(returned_ids)}"
    )
    assert duplicate_count <= EVENT_MAX_DUPLICATES, (
        "TC-NF-PERF-02 failed: duplicate count exceeded threshold. "
        f"duplicates={duplicate_count}, allowed_duplicates={EVENT_MAX_DUPLICATES}"
    )

    if failures:
        preview = "\n".join(failures[:5])
        pytest.fail(
            "TC-NF-PERF-02 failed: at least one event processing failure occurred. "
            f"failure_count={failure_count}\n{preview}"
        )


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
        "User-Agent": "SFEPS-PERF-PyTest",
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


def test_tc_nf_perf_03(stream_endpoint: RtspEndpoint, auth_endpoint):
    if STREAM_DURATION_SECONDS <= 0:
        pytest.skip("SFEPS_PERF_STREAM_DURATION_SECONDS must be > 0")
    if STREAM_POLL_INTERVAL_SECONDS <= 0:
        pytest.skip("SFEPS_PERF_STREAM_POLL_INTERVAL_SECONDS must be > 0")
    if STREAM_RECOVERY_TIMEOUT_SECONDS <= 0:
        pytest.skip("SFEPS_PERF_STREAM_RECOVERY_TIMEOUT_SECONDS must be > 0")

    if not _is_stream_ready(stream_endpoint, timeout=3.0):
        pytest.skip(
            "TC-NF-PERF-03 pre-condition 미충족: "
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
            "TC-NF-PERF-03 failed: stream outage was not recovered in allowed timeout. "
            f"timeout={STREAM_RECOVERY_TIMEOUT_SECONDS:.3f}s, "
            f"outage_count={outage_count}, endpoint={stream_endpoint.raw_url}"
        )
