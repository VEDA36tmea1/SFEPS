# Tests README

`tests/`에는 현재 pytest 기반 서버 테스트와 Squish 기반 UI 테스트가 함께 포함되어 있습니다.

- 로그인 기능: `tests/test_tc_func_login.py`
- 스트리밍 기능: `tests/test_tc_func_stream.py`
- 이벤트 판정 기능: `tests/test_tc_func_event.py`
- 스트림 UI 기능(Squish): `tests/squish/suite_sfeps/suite_sfeps/tst_tc_func_stream_02/test.py`
- UI 기능(Squish): `tests/squish/suite_sfeps/suite_sfeps/tst_tc_func_ui_01/test.py`, `tst_tc_func_ui_02/test.py`, `tst_tc_func_ui_03/test.py`
- 트래킹 기능(Squish): `tests/squish/suite_sfeps/suite_sfeps/tst_tc_func_track_01/test.py`, `tst_tc_func_track_02/test.py`
- 비기능(Recoverability): `tests/test_tc_nf_rec.py`
- 비기능(Reliability): `tests/test_tc_nf_reli.py`
- 비기능(Performance): `tests/test_tc_nf_perf.py`

`tests/conftest.py`는 세션 시작 시 `server/build/smart_server*` 바이너리로 실서버를 준비합니다.
단, EVENT 테스트(`test_tc_func_event.py`)는 실서버 기동 없이 실행됩니다.

## 현재 사용 파일

- `tests/test_tc_func_login.py`: 로그인 기능 검증
- `tests/test_tc_func_stream.py`: 스트림 수신/복구 검증(TC01, TC03)
- `tests/test_tc_func_event.py`: 이벤트 판정 로직 검증(TC-FUNC-EVENT-01~06)
- `tests/squish/suite_sfeps/suite_sfeps/tst_tc_func_stream_02/test.py`: 스트림 장애 UI 검증
- `tests/squish/suite_sfeps/suite_sfeps/tst_tc_func_ui_01/test.py`: 이벤트 목록 UI 검증
- `tests/squish/suite_sfeps/suite_sfeps/tst_tc_func_ui_02/test.py`: 상세 팝업 UI 검증
- `tests/squish/suite_sfeps/suite_sfeps/tst_tc_func_ui_03/test.py`: 로그아웃 UI 검증
- `tests/squish/suite_sfeps/suite_sfeps/tst_tc_func_track_01/test.py`: Tracking 상태 전환 검증
- `tests/squish/suite_sfeps/suite_sfeps/tst_tc_func_track_02/test.py`: Tracking 상태 표시 검증
- `tests/test_tc_nf_rec.py`: 복구성 검증
- `tests/test_tc_nf_reli.py`: 신뢰성 검증
- `tests/test_tc_nf_perf.py`: 성능 검증(TC-NF-PERF-02)
- `tests/server_event_driver.cpp`: 이벤트 판정 테스트 드라이버
- `tests/conftest.py`: 실서버 기동/종료 공통 fixture
- `tests/real_server.log`: 실서버 로그 파일

## 1) 가상환경(venv) 생성 및 준비

```bash
cd /home/iam/SFEPS
python3 -m venv .venv
source .venv/bin/activate
pip install --upgrade pip
pip install pytest
```

## 2) 실서버 기동 조건

아래 조건이 충족되어야 실서버 기반 테스트가 실행됩니다.

- 서버 바이너리 존재: `server/build/smart_server.bin` (또는 `smart_server`, `smart_server.exe`)
- DB 환경변수 설정:
  - `SFEPS_DB_USER`
  - `SFEPS_DB_PASS`
  - `SFEPS_DB_NAME_ANALYTICS`

환경변수는 쉘 export 또는 `server/.env.local`에서 제공합니다.

## 3) 구현 현황

- 명세서 기준 전체 TC: 24개
- 현재 구현 완료: 22개
- 미구현: `TC-NF-PERF-01`, `TC-SYS-01`

## 4) Functional 테스트 요약

- LOGIN (`test_tc_func_login.py`)
  - `127.0.0.1:5555` 인증 응답 검증
- STREAM (`test_tc_func_stream.py`)
  - RTSP 수신 및 장애 복구 검증
- STREAM UI (`tests/squish/.../tst_tc_func_stream_02/test.py`)
  - 스트림 장애 시 OFFLINE 상태와 배너 검증
- EVENT (`test_tc_func_event.py`)
  - 드라이버 빌드 후 실제 판정 로직 호출
  - 실서버 기동 불필요
- UI (`tests/squish/.../tst_tc_func_ui_01~03/test.py`)
  - 이벤트 목록, 상세 팝업, 로그아웃 복귀 검증
- TRACK UI (`tests/squish/.../tst_tc_func_track_01~02/test.py`)
  - Tracking ON/OFF 상태 및 상태 표시 검증

## 5) Non-Functional Recoverability 테스트 요약

- 대상: `TC-NF-REC-01` (`test_tc_nf_rec.py`)
- 인증 해제 후 강제 로그아웃 및 재로그인 복구 흐름 검증

## 6) Non-Functional Reliability 테스트 요약

- 대상: `TC-NF-RELI-01`, `TC-NF-RELI-02` (`test_tc_nf_reli.py`)
- 기본 비활성: `SFEPS_ENABLE_RELI_TESTS=1` 또는 `SFEPS_ENABLE_PERF_TESTS=1`일 때 실행
- `TC-NF-RELI-01`
  - Track/Untrack 20회 반복 안정성 검증
- `TC-NF-RELI-02`
  - 장시간 스트림 복구 신뢰성 검증

## 7) Non-Functional Performance 테스트 요약

- 대상: `TC-NF-PERF-02` (`test_tc_nf_perf.py`)
- 기본 비활성: `SFEPS_ENABLE_PERF_TESTS=1`일 때만 실행
- `TC-NF-PERF-02`
  - 이벤트 처리량/누락/중복/시간 윈도우 검증

## 8) 주요 환경변수

- Recoverability 관련
  - `SFEPS_AUTH_DEAUTH_GRACE_MS` (서버 인증 해제 유예시간, 기본 3000ms)
- Reliability 관련
  - `SFEPS_ENABLE_RELI_TESTS` (기본: `SFEPS_ENABLE_PERF_TESTS` 값 fallback)
  - `SFEPS_RELI_TRACK_TOGGLE_COUNT` (기본 `20`)
  - `SFEPS_RELI_TRACK_HOLD_SECONDS` (기본 `0.1`)
  - `SFEPS_RELI_TRACK_OBJECT_ID` (기본 `RELI-NF-01`)
  - `SFEPS_RELI_STREAM_DURATION_SECONDS` (기본: `SFEPS_PERF_STREAM_DURATION_SECONDS` 또는 `3600`)
  - `SFEPS_RELI_STREAM_POLL_INTERVAL_SECONDS` (기본: `SFEPS_PERF_STREAM_POLL_INTERVAL_SECONDS` 또는 `1`)
  - `SFEPS_RELI_STREAM_RECOVERY_TIMEOUT_SECONDS` (기본: `SFEPS_PERF_STREAM_RECOVERY_TIMEOUT_SECONDS` 또는 `10`)
- Performance 관련
  - `SFEPS_ENABLE_PERF_TESTS` (기본 `0`)
  - `SFEPS_PERF_EVENT_TARGET_COUNT` (기본 `50`)
  - `SFEPS_PERF_EVENT_WINDOW_SECONDS` (기본 `60`)
  - `SFEPS_PERF_EVENT_INTERVAL_SECONDS` (기본 `0`)
  - `SFEPS_PERF_EVENT_MAX_MISSING` (기본 `0`)
  - `SFEPS_PERF_EVENT_MAX_DUPLICATES` (기본 `0`)
- Stream/RTSP 관련
  - `SFEPS_STREAM_RTSP_URL` (기본: `RTSP_STREAM_URL` 또는 `rtsp://127.0.0.1:8554/cam1`)
  - `SFEPS_STREAM_MIN_STABLE_SECONDS` (기본 `10`)
  - `SFEPS_STREAM_RECOVERY_TIMEOUT_SECONDS` (기본 `10`)
  - `SFEPS_STREAM_RECOVERY_POLL_INTERVAL_SECONDS` (기본 `1`)
  - `SFEPS_STREAM_DOWN_DETECTION_TIMEOUT_SECONDS` (기본 `10`)
  - `SFEPS_STREAM_FAULT_DOWN_CMD`
  - `SFEPS_STREAM_FAULT_UP_CMD`
  - `SFEPS_STREAM_MANAGE_MTX_PROCESS`
  - `SFEPS_STREAM_MTX_BIN` (기본: `/home/iam/SFEPS/mediamtx/bin/mediamtx`)
  - `SFEPS_STREAM_MTX_CONFIG` (기본: `/home/iam/SFEPS/mediamtx/mediamtx.yml`)

## 9) 테스트 실행

### Linux / Pytest

로그인 테스트:

```bash
cd /home/iam/SFEPS
source .venv/bin/activate
python -m pytest -q tests/test_tc_func_login.py -r a
```

스트림 테스트:

```bash
cd /home/iam/SFEPS
source .venv/bin/activate
python -m pytest -q tests/test_tc_func_stream.py -r a
```

이벤트 테스트:

```bash
cd /home/iam/SFEPS
source .venv/bin/activate
python -m pytest -q tests/test_tc_func_event.py -r a
```

Recoverability 테스트:

```bash
cd /home/iam/SFEPS
source .venv/bin/activate
python -m pytest -q tests/test_tc_nf_rec.py -r a
```

Reliability 테스트:

```bash
cd /home/iam/SFEPS
source .venv/bin/activate
SFEPS_ENABLE_RELI_TESTS=1 python -m pytest -q tests/test_tc_nf_reli.py -r a
```

Performance 테스트:

```bash
cd /home/iam/SFEPS
source .venv/bin/activate
SFEPS_ENABLE_PERF_TESTS=1 python -m pytest -q tests/test_tc_nf_perf.py -r a
```

전체 pytest 테스트:

```bash
cd /home/iam/SFEPS
source .venv/bin/activate
python -m pytest -q tests -r a
```

### Windows / Squish

스트림 UI 테스트:

```bash
cd /home/iam/SFEPS
"/path/to/squishrunner" --testsuite tests/squish/suite_sfeps/suite_sfeps --testcase tst_tc_func_stream_02 --aut /path/to/appHanwhaVisionSFEPS
```

UI 테스트:

```bash
cd /home/iam/SFEPS
"/path/to/squishrunner" --testsuite tests/squish/suite_sfeps/suite_sfeps --testcase tst_tc_func_ui_02 --aut /path/to/appHanwhaVisionSFEPS
```

트래킹 UI 테스트:

```bash
cd /home/iam/SFEPS
"/path/to/squishrunner" --testsuite tests/squish/suite_sfeps/suite_sfeps --testcase tst_tc_func_track_01 --aut /path/to/appHanwhaVisionSFEPS
```

Jenkins 에이전트(Windows 노드) 실행 예시:

```bat
cd C:\Jenkins
java -jar agent.jar -url http://192.168.56.101:8080/ -secret @C:\Jenkins\secret-file -name "win-squish" -webSocket -workDir "C:\Jenkins"
```

## 10) 로그 확인

실서버 로그:

```bash
tail -f /home/iam/SFEPS/tests/real_server.log
```

스트림 up 기본 명령에서 `nohup` fallback이 동작한 경우:

```bash
tail -f /tmp/sfeps-mediamtx-test.log
```

EVENT 테스트는 pytest 출력 또는 driver stderr를 확인합니다.

## 11) 주의사항

- Reliability 테스트도 기본 비활성입니다. Jenkins/NF 전용 실행 시 `SFEPS_ENABLE_RELI_TESTS=1`(또는 `SFEPS_ENABLE_PERF_TESTS=1`)로 켭니다.
- Performance 테스트는 기본 비활성입니다. Jenkins 전용 실행 시 `SFEPS_ENABLE_PERF_TESTS=1`로 켭니다.
- 스트림 테스트는 실제 장애 유도를 위해 `mediamtx`를 중단/재기동할 수 있습니다.
- Squish UI 테스트는 Windows GUI 세션과 Squish 실행 환경이 필요합니다.
- 운영 장비에서 실행 시 서비스 영향이 있을 수 있으므로 테스트 환경에서 실행하세요.

## 12) 통합 테스트 실행기 (신규)

`tests/run_tests.py`로 `pytest + squish`를 프로필 기반으로 한 번에 실행할 수 있습니다.
비기능 테스트(REC/RELI/PERF)도 `nonfunctional`/`all` 프로필에서 자동 포함됩니다.

```bash
cd /home/iam/SFEPS
source .venv/bin/activate
python tests/run_tests.py --profile functional --engine both
```

### 주요 실행 예시

스모크(빠른 점검):

```bash
python tests/run_tests.py --profile smoke --engine both
```

기능 테스트만(pytest+squish):

```bash
python tests/run_tests.py --profile functional --engine both
```

비기능 테스트만(pytest):

```bash
python tests/run_tests.py --profile nonfunctional --engine pytest
```

전체 테스트:

```bash
python tests/run_tests.py --profile all --engine both
```

Squish가 없는 Linux 환경에서 pytest만:

```bash
python tests/run_tests.py --profile all --engine pytest
```

### 리포트/아티팩트

실행 결과는 기본적으로 아래 경로에 생성됩니다.

- `tests/reports/run-YYYYMMDD-HHMMSS/junit/*.xml`
- `tests/reports/run-YYYYMMDD-HHMMSS/logs/*.log`
- `tests/reports/run-YYYYMMDD-HHMMSS/test-report.html`
- `tests/reports/run-YYYYMMDD-HHMMSS/test-report.xlsx`
- `tests/reports/run-YYYYMMDD-HHMMSS/test-report.pdf`

### Squish 관련 옵션

- `--squish-runner`: `squishrunner` 실행 파일 경로
- `--squish-suite`: testsuite 경로(기본: `tests/squish/suite_sfeps/suite_sfeps`)
- `--squish-aut`: AUT 실행 파일 경로
- `--strict-squish`: Squish 실행 불가 시 skip 대신 실패 처리

예시:

```bash
python tests/run_tests.py \
  --profile functional \
  --engine both \
  --squish-runner "C:/Squish/bin/squishrunner.exe" \
  --squish-aut "C:/path/to/appHanwhaVisionSFEPS.exe"
```
