# Tests README

`tests/` 자동 테스트는 현재 아래 2개 축으로 운영됩니다.

- 로그인 기능: `tests/test_tc_func_login.py`
- 스트리밍 기능: `tests/test_tc_func_stream.py`

`tests/conftest.py`는 세션 시작 시 `server/build/smart_server*` 바이너리로 실서버를 준비합니다.

## 현재 사용 파일

- `tests/test_tc_func_login.py`: 로그인 PASS/FAIL 및 클라이언트 소스 가드 검증
- `tests/test_tc_func_stream.py`: RTSP 직접 요청 기반 스트림 검증(TC01, TC03)
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

아래 조건이 충족되어야 테스트가 실서버 기준으로 실행됩니다.

- 서버 바이너리 존재: `server/build/smart_server.bin` (또는 `smart_server`, `smart_server.exe`)
- DB 환경변수 설정:
  - `SFEPS_DB_USER`
  - `SFEPS_DB_PASS`
  - `SFEPS_DB_NAME_ANALYTICS`

환경변수는 쉘 export 또는 `server/.env.local`에서 제공합니다.

## 3) 스트림 테스트 동작 요약

- 테스트 대상: `TC-FUNC-STREAM-01`, `TC-FUNC-STREAM-03`
- `TC-FUNC-STREAM-02`는 현재 스크립트에서 제거됨
- 판정 방식:
  - `OPTIONS` / `DESCRIBE` RTSP 직접 요청
  - `DESCRIBE 200` + SDP `m=video` 기준으로 스트림 준비 상태 판단
- `TC-FUNC-STREAM-03`은 실제 장애 유도(중단/복구) 방식:
  - down 명령 실행 후 스트림 중단 감지
  - up 명령 실행 후 제한 시간 내 스트림 복구 확인

## 4) 스트림 관련 환경변수

기본값을 코드에 내장해 두었고, 필요 시 아래 변수로 override 할 수 있습니다.

- `SFEPS_STREAM_RTSP_URL`
  - 기본: `RTSP_STREAM_URL` 또는 `rtsp://127.0.0.1:8554/cam1`
- `SFEPS_STREAM_MIN_STABLE_SECONDS`
  - 기본: `10`
- `SFEPS_STREAM_RECOVERY_TIMEOUT_SECONDS`
  - 기본: `10`
- `SFEPS_STREAM_RECOVERY_POLL_INTERVAL_SECONDS`
  - 기본: `1`
- `SFEPS_STREAM_DOWN_DETECTION_TIMEOUT_SECONDS`
  - 기본: `10`
- `SFEPS_STREAM_FAULT_DOWN_CMD`
  - 기본: `systemctl stop mediamtx || sudo -n systemctl stop mediamtx || pkill -f '/mediamtx/bin/mediamtx' || true`
- `SFEPS_STREAM_FAULT_UP_CMD`
  - 기본: `systemctl start mediamtx || sudo -n systemctl start mediamtx || nohup /home/iam/SFEPS/mediamtx/run_mediamtx.sh ... &`
- `SFEPS_STREAM_MANAGE_MTX_PROCESS`
  - `1`이면 테스트가 `mediamtx/bin/mediamtx` 프로세스를 직접 관리(시작/중단/재시작)
- `SFEPS_STREAM_MTX_BIN`
  - 기본: `/home/iam/SFEPS/mediamtx/bin/mediamtx`
- `SFEPS_STREAM_MTX_CONFIG`
  - 기본: `/home/iam/SFEPS/mediamtx/mediamtx.yml`

## 5) 테스트 실행

로그인 테스트만:

```bash
cd /home/iam/SFEPS
source .venv/bin/activate
python -m pytest -q tests/test_tc_func_login.py -r a
```

스트림 테스트만:

```bash
cd /home/iam/SFEPS
source .venv/bin/activate
python -m pytest -q tests/test_tc_func_stream.py -r a
```

전체:

```bash
cd /home/iam/SFEPS
source .venv/bin/activate
python -m pytest -q tests -r a
```

## 6) 로그 확인

실서버 로그:

```bash
tail -f /home/iam/SFEPS/tests/real_server.log
```

스트림 up 기본 명령에서 `nohup` fallback이 동작한 경우:

```bash
tail -f /tmp/sfeps-mediamtx-test.log
```

## 7) 주의사항

- 스트림 테스트는 실제 장애 유도를 위해 `mediamtx`를 중단/재기동할 수 있습니다.
- 운영 장비에서 실행 시 서비스 영향이 있을 수 있으므로 테스트 환경에서 실행하세요.
