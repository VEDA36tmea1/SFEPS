# Tests README

`tests/` 자동 테스트는 현재 아래 7개 축으로 운영됩니다.

- 로그인 기능: `tests/test_tc_func_login.py`
- 스트리밍 기능: `tests/test_tc_func_stream.py`
- 이벤트 판정 기능: `tests/test_tc_func_event.py`
- 트래킹 기능: `tests/test_tc_func_track.py`
- 비기능(Recoverability): `tests/test_tc_nf_rec.py`
- 비기능(Reliability): `tests/test_tc_nf_reli.py`
- 비기능(Performance): `tests/test_tc_nf_perf.py`

`tests/conftest.py`는 세션 시작 시 `server/build/smart_server*` 바이너리로 실서버를 준비합니다.
단, EVENT 테스트(`test_tc_func_event.py`)는 파일 내부 fixture override로 실서버 기동 없이 실행됩니다.

## 현재 사용 파일

- `tests/test_tc_func_login.py`: 로그인 PASS/FAIL 및 클라이언트 소스 가드 검증
- `tests/test_tc_func_stream.py`: RTSP 직접 요청 기반 스트림 검증(TC01, TC03)
- `tests/test_tc_func_event.py`: 서버 실제 판정 로직 기반 EVENT TC-FUNC-EVENT-01~06 검증
- `tests/test_tc_func_track.py`: Track/Untrack 명령의 서버 수신(TC-FUNC-TRACK-01) 검증
- `tests/test_tc_nf_rec.py`: 서버 인증 세션 해제 후 unauthenticated 재접속 거절 및 `AUTH|FORCE_LOGOUT` 이벤트, 재로그인 복구 검증
- `tests/test_tc_nf_reli.py`: Tracking 토글 반복 안정성(TC-NF-RELI-01), 장시간 스트리밍 복구 신뢰성(TC-NF-RELI-02) 검증
- `tests/test_tc_nf_perf.py`: 이벤트 처리 성능(TC-NF-PERF-02) 검증
- `tests/server_event_driver.cpp`: `analytics.cpp`/`rfid_monitor.cpp`를 링크해 판정 로직을 호출하는 테스트 드라이버
- `tests/conftest.py`: 실서버 자동 기동/종료 및 포트(127.0.0.1:5555) 준비
- `tests/real_server.log`: 테스트 중 실서버 로그 출력 파일

## 1) 가상환경(venv) 생성 및 준비

```bash
cd /home/iam/SFEPS
python3 -m venv .venv
source .venv/bin/activate
pip install --upgrade pip
pip install pytest
```

## 2) 실서버 기동 조건 (공통)

아래 조건이 충족되어야 실서버 기반 테스트가 실행됩니다.

- 서버 바이너리 존재: `server/build/smart_server.bin` (또는 `smart_server`, `smart_server.exe`)
- DB 환경변수 설정:
  - `SFEPS_DB_USER`
  - `SFEPS_DB_PASS`
  - `SFEPS_DB_NAME_ANALYTICS`

환경변수는 쉘 export 또는 `server/.env.local`에서 제공합니다.

## 3) Functional 테스트 요약

- LOGIN (`test_tc_func_login.py`)
  - `127.0.0.1:5555` 인증 응답(PASS/FAIL) 검증
- STREAM (`test_tc_func_stream.py`)
  - RTSP `DESCRIBE 200 + m=video` 기준 검증
  - 장애 유도(down/up) 후 복구 시간 내 재연결 검증
- EVENT (`test_tc_func_event.py`)
  - `server_event_driver.cpp`를 테스트 시점 빌드 후 실제 판정 로직 호출
  - 모드: `run-case`, `parse-rfid`
  - 실서버(`smart_server`) 기동 불필요
- TRACK (`test_tc_func_track.py`)
  - 실서버 기준으로 Position 채널 `SUB_POS`/`UNSUB_POS` 전송
  - Position 연결 유지/강제 로그아웃 미발생 확인
  - 가능할 때 `tests/real_server.log`에서 서버 `SUB_POS`/`UNSUB_POS` 수신 로그 확인

## 4) Non-Functional Recoverability 테스트 요약

- 대상: `TC-NF-REC-01` (`test_tc_nf_rec.py`)
- 동작 요약:
  - 로그인 성공 및 `TEST|LOGIN_OK` 수신 확인
  - Position 연결 종료 후 인증 유예시간 경과 대기
  - 재인증 없이 Position 재접속 시 거절(unauthenticated) 확인
  - Alert 채널에서 `AUTH|FORCE_LOGOUT|REASON=POSITION_UNAUTHENTICATED` 수신 확인
  - 재로그인 후 Position 재접속 정상 복귀 확인

## 5) Non-Functional Reliability 테스트 요약

- 대상: `TC-NF-RELI-01`, `TC-NF-RELI-02` (`test_tc_nf_reli.py`)
- 기본 비활성: `SFEPS_ENABLE_RELI_TESTS=1` 또는 `SFEPS_ENABLE_PERF_TESTS=1`일 때 실행
- `TC-NF-RELI-01`
  - 실서버 기준으로 `SUB_POS`(Track) / `UNSUB_POS`(Untrack) 20회 반복
  - 반복 중 Position 연결 비정상 종료/`AUTH|FORCE_LOGOUT` 발생 여부 검증
  - 반복 후 재인증 및 Position 재접속 정상 여부 검증
  - 가능할 때 `tests/real_server.log`에서 `SUB_POS`/`UNSUB_POS` 수신 로그 최소 횟수 검증
- `TC-NF-RELI-02`
  - 장시간 스트림 모니터링 중 장애 발생 시 허용 복구 시간 내 회복 여부 검증

## 6) Non-Functional Performance 테스트 요약

- 대상: `TC-NF-PERF-02` (`test_tc_nf_perf.py`)
- 기본 비활성: `SFEPS_ENABLE_PERF_TESTS=1`일 때만 실행
- `TC-NF-PERF-02`
  - 이벤트 드라이버 반복 실행으로 처리량/누락/중복/시간 윈도우 검증

## 7) 주요 환경변수

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
- Functional Tracking 관련
  - `SFEPS_TRACK_CMD_TIMEOUT_SECONDS` (기본 `3.0`)
  - `SFEPS_TRACK_OBJECT_ID` (기본 `FUNC-TRACK-01`)
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

## 8) 테스트 실행

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

트래킹 테스트:

```bash
cd /home/iam/SFEPS
source .venv/bin/activate
python -m pytest -q tests/test_tc_func_track.py -r a
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

전체 테스트:

```bash
cd /home/iam/SFEPS
source .venv/bin/activate
python -m pytest -q tests -r a
```

## 9) 로그 확인

실서버 로그:

```bash
tail -f /home/iam/SFEPS/tests/real_server.log
```

스트림 up 기본 명령에서 `nohup` fallback이 동작한 경우:

```bash
tail -f /tmp/sfeps-mediamtx-test.log
```

EVENT 테스트는 실서버 로그 대신 pytest 출력/driver stderr를 확인합니다.

## 10) 주의사항

- Reliability 테스트도 기본 비활성입니다. Jenkins/NF 전용 실행 시 `SFEPS_ENABLE_RELI_TESTS=1`(또는 `SFEPS_ENABLE_PERF_TESTS=1`)로 켭니다.
- Performance 테스트는 기본 비활성입니다. Jenkins 전용 실행 시 `SFEPS_ENABLE_PERF_TESTS=1`로 켭니다.
- 스트림 테스트는 실제 장애 유도를 위해 `mediamtx`를 중단/재기동할 수 있습니다.
- 운영 장비에서 실행 시 서비스 영향이 있을 수 있으므로 테스트 환경에서 실행하세요.
