# SFEPS Security Hardening Summary

최종 갱신: 2026-03-19

## 1) 적용 목표

현재 서버 보안 강화 목표는 다음과 같습니다.
- DB 로컬 전용 접근 강제 (`localhost` only)
- 앱 서비스 인입 제어(allowlist 기반 fail-closed)
- 로그인 무차별 대입 완화(5회 실패 30초 락아웃)
- TLS 점진 전환 시 fail-closed 검증(인증서/키/포트 충돌 검증)

## 2) 현재 적용된 보안 정책

### A. DB 로컬 전용 정책
- 적용 코드:
  - `server/src/runtime_config.cpp`
  - `server/run_server.sh`
- 동작:
  - `SFEPS_DB_HOST` 미설정 시 `localhost`
  - `localhost` 외 값이면 기동 실패

### B. 앱 포트 allowlist fail-closed
- 적용 코드:
  - `server/src/security_runtime.cpp`
  - `server/run_server.sh`
  - `server/src/services/transport_utils.cpp`
- 동작:
  - `SFEPS_AUTH_ALLOW_IPS`, `SFEPS_AUDIO_ALLOW_IPS`, `SFEPS_ALERT_ALLOW_IPS`는 필수
  - 미설정/빈 값이면 기동 실패
  - Position/VideoCatalog 서비스는 현재 `SFEPS_ALERT_ALLOW_IPS` 정책을 공유

### C. 로그인 락아웃 정책 (서버 측)
- 적용 코드: `server/src/services/auth_service.cpp`
- 기준: `계정 + 클라이언트 IP`
- 규칙:
  - 5회 연속 실패 시 30초 잠금
  - 잠금 중 요청은 DB 인증 없이 `FAIL`
  - 성공 로그인 시 실패 카운트 초기화
  - 비활성 키는 주기적으로 정리

### D. 강제 로그아웃 이벤트 (Position 비인증 접속)
- 적용 코드: `server/src/services/position_service.cpp`
- 동작:
  - 비인증 IP가 Position 접속 시 거절
  - Alert 채널로 해당 IP 대상 `AUTH|FORCE_LOGOUT|...` 전송
  - IP 단위 5초 쿨다운 적용

### E. TLS 모드 검증
- 적용 코드:
  - `server/src/security_runtime.cpp`
  - `server/run_server.sh`
- 동작:
  - TLS 모드(`SFEPS_APP_TLS_ENABLE=1`)에서 cert/key 필수 및 읽기 가능해야 함
  - TLS 포트(6555~6559) 상호 중복 금지
  - Plain+TLS 동시 운영 시 Plain 포트(5555~5559)와 TLS 포트 충돌 금지

## 3) 비밀정보 분리/회전 상태

- DB 접속정보 하드코딩 제거, 런타임 env 로드 방식 유지
- 런타임 필수 DB env:
  - `SFEPS_DB_USER`
  - `SFEPS_DB_PASS`
  - `SFEPS_DB_NAME_ANALYTICS`
- `SFEPS_DB_NAME_AUTH`는 호환 목적 env이며 런타임 단일 스키마(`SFEPS_DB_NAME_ANALYTICS`)를 사용

## 4) 런타임 필수 환경변수

```bash
# DB (fail-closed)
SFEPS_DB_HOST=localhost
SFEPS_DB_USER=pi
SFEPS_DB_PASS=...rotated...
SFEPS_DB_NAME_ANALYTICS=CCgbd

# Allowlist (fail-closed)
SFEPS_AUTH_ALLOW_IPS=192.168.0.10
SFEPS_AUDIO_ALLOW_IPS=192.168.0.11
SFEPS_ALERT_ALLOW_IPS=192.168.0.10,192.168.0.12
```

TLS를 켜는 경우 추가 필수:

```bash
SFEPS_APP_TLS_ENABLE=1
SFEPS_APP_TLS_CERT_FILE=/etc/sfeps/pki/server.crt
SFEPS_APP_TLS_KEY_FILE=/etc/sfeps/pki/server.key
```

## 5) 실행/자동시작 구조

### 실행 스크립트
- 파일: `server/run_server.sh`
- 역할:
  - env 파일 자동 로드(`.env.local`/`.env`)
  - 필수 env 검증
  - allowlist fail-closed 검증
  - TLS 모드 검증
  - 서버 바이너리 실행

### systemd 서비스
- 유닛: `/etc/systemd/system/sfeps-server.service`
- env 파일(운영): `/etc/default/sfeps-server` 또는 컨테이너 env 파일

## 6) 운영 체크 명령어

```bash
systemctl is-active mariadb
systemctl is-active mediamtx
systemctl is-active sfeps-server
systemctl is-enabled sfeps-server
```

```bash
journalctl -u sfeps-server -f
journalctl -u sfeps-server -b | tail -n 200
```

```bash
mysql -h localhost -u pi -p -e "SELECT 1;"
```

## 7) 회전(비밀번호 변경) 절차

1. MariaDB 사용자 비밀번호 변경
2. 서버 env 파일의 `SFEPS_DB_PASS` 갱신
3. `sudo systemctl restart sfeps-server`
4. 로그인/DB 연결/기동 로그 확인

## 8) 의도된 실패 시나리오

아래 오류는 fail-closed 정책상 정상 동작입니다.
- `missing required env: SFEPS_DB_PASS`
- `SFEPS_DB_HOST must be localhost (local-only mode)`
- `missing required allowlist: SFEPS_AUTH_ALLOW_IPS (fail-closed)`
- `missing required env for TLS mode: SFEPS_APP_TLS_CERT_FILE/SFEPS_APP_TLS_KEY_FILE`

## 9) 별도 이슈 (보안 정책과 분리)

아래 로그는 별도 운영 이슈 범주입니다.
- `rc522 socket connect: No such file or directory`
- `rc522 socket connect: Permission denied`
- `Address already in use`
