import json
import subprocess
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
EVENT_DRIVER_SRC = REPO_ROOT / "tests" / "server_event_driver.cpp"
EVENT_DRIVER_BIN_DIR = REPO_ROOT / "tests" / ".bin"
EVENT_DRIVER_BIN = EVENT_DRIVER_BIN_DIR / "server_event_driver"


@pytest.fixture(scope="session", autouse=True)
def real_auth_server():
    """Override global autouse fixture: event tests do not require smart_server process."""
    yield


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


def test_tc_func_event_01(
    event_driver_bin,
):
    res = _run_driver_json(event_driver_bin, "run-case", "30", "__EMPTY__", "OBJ-E01")
    assert res["fraud"] is True


def test_tc_func_event_02(event_driver_bin):
    case_a = _run_driver_json(event_driver_bin, "run-case", "19", "youth", "OBJ-E02-A")
    case_b = _run_driver_json(event_driver_bin, "run-case", "20", "youth", "OBJ-E02-B")

    assert case_a["fraud"] is False
    assert case_b["fraud"] is True


def test_tc_func_event_03(event_driver_bin):
    case_a = _run_driver_json(event_driver_bin, "run-case", "59", "senior", "OBJ-E03-A")
    case_b = _run_driver_json(event_driver_bin, "run-case", "60", "senior", "OBJ-E03-B")

    assert case_a["fraud"] is True
    assert case_b["fraud"] is False


def test_tc_func_event_04(event_driver_bin):
    res = _run_driver_json(event_driver_bin, "run-case", "20", "youth", "OBJ-E04")
    assert res["fraud"] is True
    assert res["queue_size"] >= 1
    assert res["object_id"] == "OBJ-E04"


def test_tc_func_event_05(event_driver_bin):
    res = _run_driver_json(event_driver_bin, "run-case", "19", "youth", "OBJ-E05")
    assert res["fraud"] is False


def test_tc_func_event_06(
    event_driver_bin,
):
    malformed_messages = [
        '{"id":"A1",',  # broken json
        '{"id":123,"text":"youth"}',  # type mismatch
        '{"text":"senior"}',  # missing id
    ]

    for raw in malformed_messages:
        _ = _run_driver_json(event_driver_bin, "parse-rfid", raw)

    valid = '{"id":"OK-1","text":"senior"}'
    parsed = _run_driver_json(event_driver_bin, "parse-rfid", valid)
    assert parsed["id"] == "OK-1"
    assert parsed["text"] == "senior"
