import json
import os
import socket
import subprocess
import time
from pathlib import Path

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
