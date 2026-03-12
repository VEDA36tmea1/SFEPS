import json
import os
import subprocess
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
EVENT_DRIVER_SRC = REPO_ROOT / "tests" / "server_event_driver.cpp"
EVENT_DRIVER_BIN_DIR = REPO_ROOT / "tests" / ".bin"
EVENT_DRIVER_BIN = EVENT_DRIVER_BIN_DIR / "server_event_driver"


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


def test_tc_func_event_01_no_tagged_card_is_marked_as_fare_evasion_and_alerted_to_client(
    event_driver_bin,
):
    res = _run_driver_json(event_driver_bin, "run-case", "30th", "__EMPTY__", "OBJ-E01")
    assert res["fraud"] is True
    assert res["queue_size"] >= 1
    assert res["alert_count"] >= 1


def test_tc_func_event_02_youth_boundary_19_and_20_is_consistent(event_driver_bin):
    case_a = _run_driver_json(event_driver_bin, "run-case", "19th", "youth", "OBJ-E02-A")
    case_b = _run_driver_json(event_driver_bin, "run-case", "20th", "youth", "OBJ-E02-B")

    assert case_a["fraud"] is False
    assert case_b["fraud"] is True


def test_tc_func_event_03_senior_boundary_59_and_60_is_consistent(event_driver_bin):
    case_a = _run_driver_json(event_driver_bin, "run-case", "59th", "senior", "OBJ-E03-A")
    case_b = _run_driver_json(event_driver_bin, "run-case", "60th", "senior", "OBJ-E03-B")

    assert case_a["fraud"] is True
    assert case_b["fraud"] is False


def test_tc_func_event_04_suspicious_case_creates_event_log(event_driver_bin):
    res = _run_driver_json(event_driver_bin, "run-case", "20th", "youth", "OBJ-E04")
    assert res["fraud"] is True
    assert res["queue_size"] >= 1
    assert res["object_id"] == "OBJ-E04"


def test_tc_func_event_05_normal_case_does_not_create_event_log(event_driver_bin):
    res = _run_driver_json(event_driver_bin, "run-case", "19th", "youth", "OBJ-E05")
    assert res["fraud"] is False
    assert res["queue_size"] == 0
    assert res["alert_count"] == 0


def test_tc_func_event_06_suspicious_event_is_saved_with_required_db_fields(event_driver_bin):
    if not (
        os.getenv("SFEPS_DB_USER")
        and os.getenv("SFEPS_DB_PASS")
        and os.getenv("SFEPS_DB_NAME_ANALYTICS")
    ):
        pytest.skip("TC-FUNC-EVENT-06 requires DB env vars")

    res = _run_driver_json(event_driver_bin, "run-db-case", "59th", "senior", "OBJ-E06")
    if "skipped" in res:
        pytest.skip(res["skipped"])

    assert res["fraud"] is True
    assert res["inserted_count"] >= 1
    assert res["required_fields_ok"] is True


def test_tc_func_event_07_malformed_event_message_is_ignored_without_crash_and_normal_message_still_works(
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
