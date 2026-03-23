#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
DEFAULT_REPORTS_DIR = TESTS_DIR / "reports"
DEFAULT_SQUISH_SUITE = TESTS_DIR / "squish" / "suite_sfeps" / "suite_sfeps"

PYTEST_PROFILES = {
    "smoke": [
        "tests/test_tc_func_login.py",
        "tests/test_tc_func_event.py",
    ],
    "functional": [
        "tests/test_tc_func_login.py",
        "tests/test_tc_func_stream.py",
        "tests/test_tc_func_event.py",
    ],
    "nonfunctional": [
        "tests/test_tc_nf_rec.py",
        "tests/test_tc_nf_reli.py",
        "tests/test_tc_nf_perf.py",
    ],
}
PYTEST_PROFILES["all"] = PYTEST_PROFILES["functional"] + PYTEST_PROFILES["nonfunctional"]

SQUISH_PROFILES = {
    "smoke": [
        "tst_tc_func_ui_01",
    ],
    "functional": [
        "tst_tc_func_stream_02",
        "tst_tc_func_ui_01",
        "tst_tc_func_ui_02",
        "tst_tc_func_ui_03",
        "tst_tc_func_track_01",
        "tst_tc_func_track_02",
    ],
    "nonfunctional": [],
}
SQUISH_PROFILES["all"] = SQUISH_PROFILES["functional"] + SQUISH_PROFILES["nonfunctional"]

NF_RELI_TEST = "tests/test_tc_nf_reli.py"
NF_PERF_TEST = "tests/test_tc_nf_perf.py"


@dataclass
class StepResult:
    name: str
    status: str
    return_code: int
    duration_sec: float
    log_path: Path
    junit_path: Path


def now_stamp() -> str:
    return datetime.now().strftime("%Y%m%d-%H%M%S")


def ensure_dir(path: Path) -> Path:
    path.mkdir(parents=True, exist_ok=True)
    return path


def normalize_name(name: str) -> str:
    safe = []
    for ch in name:
        safe.append(ch if ch.isalnum() or ch in ("-", "_", ".") else "_")
    return "".join(safe).strip("_") or "step"


def log_tail(path: Path, max_lines: int = 80) -> str:
    if not path.is_file():
        return ""
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    return "\n".join(lines[-max_lines:])


def write_junit(step: StepResult, classname: str, message: str = "") -> None:
    tests = 1
    failures = 1 if step.status == "failed" else 0
    skipped = 1 if step.status == "skipped" else 0
    errors = 0

    suite = ET.Element(
        "testsuite",
        attrib={
            "name": classname,
            "tests": str(tests),
            "failures": str(failures),
            "errors": str(errors),
            "skipped": str(skipped),
            "time": f"{step.duration_sec:.3f}",
        },
    )
    case = ET.SubElement(
        suite,
        "testcase",
        attrib={
            "classname": classname,
            "name": step.name,
            "time": f"{step.duration_sec:.3f}",
        },
    )

    if step.status == "failed":
        failure = ET.SubElement(case, "failure", attrib={"message": message or f"Exit code {step.return_code}"})
        failure.text = log_tail(step.log_path)
    elif step.status == "skipped":
        skipped_node = ET.SubElement(case, "skipped", attrib={"message": message or "Skipped by runner"})
        skipped_node.text = message

    tree = ET.ElementTree(suite)
    step.junit_path.parent.mkdir(parents=True, exist_ok=True)
    tree.write(step.junit_path, encoding="utf-8", xml_declaration=True)


def stream_run(cmd: list[str], env: dict[str, str], log_path: Path, dry_run: bool) -> tuple[int, float]:
    start = time.monotonic()
    log_path.parent.mkdir(parents=True, exist_ok=True)

    with log_path.open("w", encoding="utf-8") as fp:
        fp.write("$ " + " ".join(cmd) + "\n\n")
        if dry_run:
            fp.write("[dry-run] command skipped\n")
            return 0, time.monotonic() - start

        proc = subprocess.Popen(
            cmd,
            cwd=str(REPO_ROOT),
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )

        assert proc.stdout is not None
        for line in proc.stdout:
            sys.stdout.write(line)
            fp.write(line)

        rc = proc.wait()
        return rc, time.monotonic() - start


def run_pytest_step(
    test_file: str,
    base_env: dict[str, str],
    logs_dir: Path,
    junit_dir: Path,
    pytest_extra: list[str],
    dry_run: bool,
) -> StepResult:
    file_path = REPO_ROOT / test_file
    step_name = f"pytest::{Path(test_file).name}"
    safe = normalize_name(step_name)
    log_path = logs_dir / f"{safe}.log"
    junit_path = junit_dir / f"{safe}.xml"

    env = dict(base_env)
    if test_file == NF_RELI_TEST:
        env.setdefault("SFEPS_ENABLE_RELI_TESTS", "1")
    if test_file == NF_PERF_TEST:
        env.setdefault("SFEPS_ENABLE_PERF_TESTS", "1")
        env.setdefault("SFEPS_ENABLE_RELI_TESTS", "1")

    if not file_path.is_file():
        result = StepResult(step_name, "skipped", 0, 0.0, log_path, junit_path)
        write_junit(result, "pytest", message=f"Missing test file: {test_file}")
        return result

    cmd = [
        sys.executable,
        "-m",
        "pytest",
        "-q",
        test_file,
        "-r",
        "a",
        "--junitxml",
        str(junit_path),
    ] + pytest_extra

    rc, duration = stream_run(cmd, env, log_path, dry_run=dry_run)
    if dry_run:
        result = StepResult(step_name, "skipped", 0, duration, log_path, junit_path)
        write_junit(result, "pytest", message="Dry-run: pytest step was not executed")
        return result

    status = "passed" if rc == 0 else "failed"
    return StepResult(step_name, status, rc, duration, log_path, junit_path)


def find_squish_runner(explicit_runner: str | None) -> str:
    if explicit_runner:
        return explicit_runner
    from_env = os.environ.get("SFEPS_SQUISH_RUNNER") or os.environ.get("SQUISH_RUNNER")
    if from_env:
        return from_env
    found = shutil.which("squishrunner")
    return found or ""


def run_squish_step(
    testcase: str,
    base_env: dict[str, str],
    logs_dir: Path,
    junit_dir: Path,
    squish_runner: str,
    squish_suite: Path,
    squish_aut: str | None,
    dry_run: bool,
) -> StepResult:
    step_name = f"squish::{testcase}"
    safe = normalize_name(step_name)
    log_path = logs_dir / f"{safe}.log"
    junit_path = junit_dir / f"{safe}.xml"

    cmd = [
        squish_runner,
        "--testsuite",
        str(squish_suite),
        "--testcase",
        testcase,
    ]
    if squish_aut:
        cmd += ["--aut", squish_aut]

    rc, duration = stream_run(cmd, base_env, log_path, dry_run=dry_run)
    if dry_run:
        result = StepResult(step_name, "skipped", 0, duration, log_path, junit_path)
        write_junit(result, "squish", message="Dry-run: squish step was not executed")
        return result

    status = "passed" if rc == 0 else "failed"
    result = StepResult(step_name, status, rc, duration, log_path, junit_path)
    write_junit(result, "squish", message=f"Squish testcase failed: {testcase}")
    return result

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="SFEPS 통합 테스트 실행기 (pytest + squish)")
    parser.add_argument(
        "--profile",
        choices=["smoke", "functional", "nonfunctional", "all"],
        default="functional",
        help="실행할 테스트 프로필",
    )
    parser.add_argument(
        "--engine",
        choices=["pytest", "squish", "both"],
        default="both",
        help="실행 엔진 선택",
    )
    parser.add_argument(
        "--reports-dir",
        default=str(DEFAULT_REPORTS_DIR),
        help="리포트 루트 디렉터리",
    )
    parser.add_argument(
        "--run-name",
        default=f"run-{now_stamp()}",
        help="실행 결과 디렉터리 이름",
    )
    parser.add_argument(
        "--fail-fast",
        action="store_true",
        help="한 단계 실패 시 즉시 종료",
    )
    parser.add_argument(
        "--no-report",
        action="store_true",
        help="HTML/XLS/XLSX/PDF 리포트 생성 생략",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="실행하지 않고 계획만 출력",
    )
    parser.add_argument(
        "--squish-runner",
        default="",
        help="squishrunner 경로(미지정 시 env/which 탐색)",
    )
    parser.add_argument(
        "--squish-suite",
        default=os.environ.get("SFEPS_SQUISH_SUITE", str(DEFAULT_SQUISH_SUITE)),
        help="Squish suite 경로",
    )
    parser.add_argument(
        "--squish-aut",
        default=os.environ.get("SFEPS_SQUISH_AUT", ""),
        help="Squish AUT 경로(필요 시)",
    )
    parser.add_argument(
        "--strict-squish",
        action="store_true",
        help="Squish 실행 불가 시 실패 처리",
    )
    parser.add_argument(
        "--pytest-extra",
        default="",
        help='pytest 추가 인자 문자열 (예: "-k login -m smoke")',
    )
    return parser.parse_args()


def run_report_generator(base_env: dict[str, str], junit_dir: Path, run_dir: Path, dry_run: bool) -> int:
    report_script = REPO_ROOT / "scripts" / "generate_test_reports.py"
    if not report_script.is_file():
        print(f"[WARN] report script not found: {report_script}")
        return 0

    cmd = [
        sys.executable,
        str(report_script),
        "--input",
        str(junit_dir),
        "--output",
        str(run_dir),
    ]
    report_log = run_dir / "logs" / "report-generator.log"
    rc, _ = stream_run(cmd, base_env, report_log, dry_run=dry_run)
    return rc


def print_summary(results: list[StepResult], run_dir: Path) -> None:
    print("\n=== SFEPS Test Runner Summary ===")
    for r in results:
        print(f"- {r.status.upper():7} {r.name:40} ({r.duration_sec:6.2f}s)  log={r.log_path}")
    passed = sum(1 for r in results if r.status == "passed")
    failed = sum(1 for r in results if r.status == "failed")
    skipped = sum(1 for r in results if r.status == "skipped")
    print(f"\nTotals: passed={passed}, failed={failed}, skipped={skipped}, total={len(results)}")
    print(f"Artifacts: {run_dir}")


def main() -> int:
    args = parse_args()
    base_env = os.environ.copy()
    run_dir = ensure_dir(Path(args.reports_dir) / args.run_name)
    junit_dir = ensure_dir(run_dir / "junit")
    logs_dir = ensure_dir(run_dir / "logs")
    pytest_extra = [part for part in args.pytest_extra.split(" ") if part.strip()]
    results: list[StepResult] = []

    want_pytest = args.engine in ("pytest", "both")
    want_squish = args.engine in ("squish", "both")

    print(f"[INFO] profile={args.profile} engine={args.engine}")
    print(f"[INFO] run_dir={run_dir}")

    if want_pytest:
        for test_file in PYTEST_PROFILES[args.profile]:
            result = run_pytest_step(test_file, base_env, logs_dir, junit_dir, pytest_extra, dry_run=args.dry_run)
            results.append(result)
            if result.status == "failed" and args.fail_fast:
                print("[ERROR] fail-fast enabled; stopping after pytest failure.")
                print_summary(results, run_dir)
                return 1

    if want_squish:
        squish_runner = find_squish_runner(args.squish_runner or None)
        suite_path = Path(args.squish_suite)
        squish_aut = args.squish_aut or None

        can_run_squish = bool(squish_runner) and suite_path.is_dir()
        if not can_run_squish:
            message = (
                "Squish skipped: runner/suite missing "
                f"(runner='{squish_runner}', suite='{suite_path}')"
            )
            print(f"[WARN] {message}")
            for testcase in SQUISH_PROFILES[args.profile]:
                step_name = f"squish::{testcase}"
                safe = normalize_name(step_name)
                result = StepResult(
                    name=step_name,
                    status="failed" if args.strict_squish else "skipped",
                    return_code=1 if args.strict_squish else 0,
                    duration_sec=0.0,
                    log_path=logs_dir / f"{safe}.log",
                    junit_path=junit_dir / f"{safe}.xml",
                )
                write_junit(result, "squish", message=message)
                results.append(result)
            if args.strict_squish and SQUISH_PROFILES[args.profile]:
                print_summary(results, run_dir)
                return 1
        else:
            for testcase in SQUISH_PROFILES[args.profile]:
                result = run_squish_step(
                    testcase=testcase,
                    base_env=base_env,
                    logs_dir=logs_dir,
                    junit_dir=junit_dir,
                    squish_runner=squish_runner,
                    squish_suite=suite_path,
                    squish_aut=squish_aut,
                    dry_run=args.dry_run,
                )
                results.append(result)
                if result.status == "failed" and args.fail_fast:
                    print("[ERROR] fail-fast enabled; stopping after squish failure.")
                    print_summary(results, run_dir)
                    return 1

    if not args.no_report:
        report_rc = run_report_generator(base_env, junit_dir, run_dir, dry_run=args.dry_run)
        if report_rc != 0:
            print("[WARN] report generation failed; see logs/report-generator.log")

    print_summary(results, run_dir)

    has_failures = any(r.status == "failed" for r in results)
    return 1 if has_failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
