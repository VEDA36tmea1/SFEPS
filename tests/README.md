# tests 실행 가이드

이 문서는 `TC-NF-PERF-02` 자동 테스트(`pytest`) 실행 방법을 설명합니다.
상세 테스트케이스 정의는 `tests/TestCase.md`를 참고하세요.

## 1) 어디서 실행하나요?
- **실행 위치(Workspace Root)**: `c:\Users\2-08\Desktop\SFEPS`
- 아래 명령은 모두 PowerShell 기준입니다.

## 2) 무엇을 실행하나요?
- 대상 테스트 파일: `tests/test_tc_nf_perf_02.py`
- 목적: **1분 내 20건 이상 의심 이벤트 처리(생성/전달/표시)** 검증

## 3) 사전 준비

### 3-1. 가상환경(.venv) 인터프리터 사용
- 테스트 실행 인터프리터:
  - `c:/Users/2-08/Desktop/SFEPS/.venv/Scripts/python.exe`

### 3-2. UDS 주입기 설정 (테스트 스크립트 내장)
`test_tc_nf_perf_02.py`가 RC522 데몬 포맷 NDJSON을 직접 생성해 `/tmp/rc522_events.sock`으로 전송합니다.

옵션(기본값 있음):

```powershell
$env:SFEPS_PERF_RFID_SOCKET_PATH="/tmp/rc522_events.sock"
$env:SFEPS_PERF_RFID_TEXT="Invalid"
$env:SFEPS_PERF_RFID_UID_SEED="2684354560"  # 0xA0000000
$env:SFEPS_PERF_RFID_DEVICE_ID="1"
$env:SFEPS_PERF_RFID_SEND_INTERVAL_SEC="0.05"
$env:SFEPS_PERF_RFID_ACCEPT_TIMEOUT_SEC="10"
```

주의: `uds` 모드는 Linux/Unix(AF_UNIX) 환경에서만 동작합니다.

### 3-3. 선택 환경변수(기본값 있음)
필요할 때만 설정하세요.

```powershell
$env:SFEPS_PERF_ALERT_HOST="192.168.0.92"   # 미설정 시 FRAUD_SERVER_HOST 또는 192.168.0.92
$env:SFEPS_PERF_ALERT_PORT="5557"           # 미설정 시 5557
$env:SFEPS_PERF_TARGET_COUNT="20"
$env:SFEPS_PERF_WINDOW_SEC="60"
$env:SFEPS_PERF_ALLOWED_MISSING="0"
$env:SFEPS_PERF_ALLOWED_DUPLICATE="0"
$env:SFEPS_PERF_MESSAGE_PREFIX="FRAUD|"
```

## 4) 실행 명령

```powershell
c:/Users/2-08/Desktop/SFEPS/.venv/Scripts/python.exe -m pytest tests/test_tc_nf_perf_02.py -q
```

## 5) 결과 해석
- `1 passed`: 성능 기준 충족
- `1 skipped`: Windows 환경(UDS 미지원)
- `1 failed`: 시간 초과 또는 알림 채널 수신 건수 부족, 누락/중복 허용치 초과

## 6) 테스트 동작 방식
- 테스트는 SFEPS 알림 채널(TCP, 기본 `192.168.0.92:5557`)에 클라이언트로 접속합니다.
- 테스트 스크립트가 RC522 NDJSON(`device_id`, `id`, `text`, `timestamp`)를 직접 전송합니다.
- 수신 메시지 중 `FRAUD|`(기본 prefix)로 시작하는 라인을 집계해 성능 기준을 검증합니다.

## 7) 빠른 점검 팁
- 현재 셸에서 환경변수 확인:

```powershell
Get-ChildItem Env:SFEPS_PERF_*
```

- 환경변수 초기화(현재 셸만):

```powershell
Remove-Item Env:SFEPS_PERF_ALERT_HOST, Env:SFEPS_PERF_ALERT_PORT -ErrorAction SilentlyContinue
```

## 8) Jenkins(VM)에서 실행
- 저장소 루트의 `Jenkinsfile`이 `tests/test_tc_nf_perf_02.py`를 실행하도록 구성되어 있습니다.
- Jenkins Job이 GitHub 저장소를 빌드하도록 연결되어 있으면, 별도 스크립트 없이 `Build Now`로 실행됩니다.

### Jenkins 실행 전제(중요)
- Jenkins 에이전트는 **Linux/Unix**여야 합니다(UDS `/tmp/rc522_events.sock` 사용).
- 같은 VM/호스트에서 SFEPS 서버가 실행 중이어야 합니다.
  - 서버의 Alert 리스너(TCP 5557 기본) 동작 필요
  - 서버의 RFID 모니터가 `/tmp/rc522_events.sock`로 접속 시도 중이어야 함

### Jenkins 파라미터(필요 시)
- `SFEPS_PERF_ALERT_HOST` (기본 `127.0.0.1`)
- `SFEPS_PERF_ALERT_PORT` (기본 `5557`)
- `SFEPS_PERF_TARGET_COUNT` (기본 `20`)
- `SFEPS_PERF_WINDOW_SEC` (기본 `60`)
- `SFEPS_PERF_RFID_SOCKET_PATH` (기본 `/tmp/rc522_events.sock`)

### 결과 확인
- Jenkins Test Result: `reports/pytest_tc_nf_perf_02.xml`
- 콘솔 로그에서 `passed/failed/skipped` 확인
