# SFEPS Security Hardening Summary

최종 갱신: 2026-02-25

## 1) 적용 목표

현재 서버 보안 강화의 목적은 다음 3가지입니다.
- DB 로컬 전용 접근 강제 (`localhost`만 허용)
- RTSPS 인증서 검증 강제 (`tls_verify=0` 제거)
- 로그인 보안 강화 (서버측 5회 실패 30초 락아웃)

## 2) 현재 적용된 보안 정책

### A. DB 로컬 전용 정책
- 적용 컴포넌트: `auth.cpp`, `log.cpp`, `analytics.cpp`
- 핵심 동작:
  - `SFEPS_DB_HOST`는 `localhost`만 허용
  - 미설정 시 기본값 `localhost` 적용

### B. RTSPS 인증서 검증 강제
- 적용 코드: `server/src/recorder.cpp`
- 핵심 동작:
  - `RTSPS_TLS_CA` 미설정/읽기불가 시 즉시 실패
  - `tls_verify=1`
  - `ca_file=<RTSPS_TLS_CA>`
  - `verifyhost=192.168.0.97`

### C. 로그인 락아웃 정책 (서버 측)
- 적용 코드: `server/src/main.cpp` (`run_login_auth`)
- 기준: `계정 + 클라이언트 IP`
- 규칙:
  - 5회 연속 실패 시 30초 잠금
  - 잠금 중 요청은 DB 인증 호출 없이 즉시 `FAIL`
  - 성공 로그인 시 해당 키의 실패 카운트 초기화
- 저장 방식: 메모리 (서버 재시작 시 초기화)

## 3) 비밀정보 분리/회전 상태

### 하드코딩 제거 상태
- 기존 `main.cpp`의 `DB_HOST/DB_USER/DB_PASS/DB_NAME` 하드코딩 제거
- DB 접속정보는 런타임 env에서만 로드

### 런타임 설정 로더
- 헤더: `server/include/runtime_config.h`
- 구현: `server/src/runtime_config.cpp`
- fail-closed 필수 env:
  - `SFEPS_DB_USER`
  - `SFEPS_DB_PASS`
  - `SFEPS_DB_NAME_AUTH`
  - `SFEPS_DB_NAME_ANALYTICS`
- DB host 정책:
  - `SFEPS_DB_HOST` 미설정 시 `localhost`
  - `localhost` 외 값은 즉시 실패

### DBLogger 하드코딩 제거
- `DBLogger` 생성자 인자로 host/user/pass/db 주입
- `server/include/log.h`, `server/src/log.cpp` 반영

## 4) 런타임 필수 환경변수

```bash
# TLS 검증용
RTSPS_TLS_CA=/etc/sfeps/pki/ca.crt

# DB 접속정보 (fail-closed)
SFEPS_DB_HOST=localhost
SFEPS_DB_USER=pi
SFEPS_DB_PASS=...회전된비밀번호...
SFEPS_DB_NAME_AUTH=Client_db
SFEPS_DB_NAME_ANALYTICS=CCgbd
```

## 5) 실행/자동시작 구조

### 실행 스크립트
- 파일: `server/run_server.sh`
- 역할:
  - RTSPS TLS env 검사
  - DB host 로컬 전용 정책 검사
  - DB env 필수값 검사
  - 누락 시 즉시 종료(fail-closed)
  - `smart_server` 실행

### systemd 서비스
- 유닛: `/etc/systemd/system/sfeps-server.service`
- env 파일: `/etc/default/sfeps-server`
- 상태: 부팅 자동시작(`enabled`) 기준 운영

## 6) 운영 체크 명령어

### 서비스 상태
```bash
systemctl is-active mariadb
systemctl is-active mediamtx
systemctl is-active sfeps-server
systemctl is-enabled sfeps-server
```

### 서버 로그
```bash
journalctl -u sfeps-server -f
journalctl -u sfeps-server -b | tail -n 100
```

### DB 로컬 연결 검증
```bash
mysql -h localhost -u pi -p -e "SELECT 1;"
```

## 7) 회전(비밀번호 변경) 절차

1. MariaDB 사용자 비밀번호 변경
2. `/etc/default/sfeps-server`의 `SFEPS_DB_PASS` 갱신
3. `sudo systemctl restart sfeps-server`
4. 로그인/DB 연결 정상 여부 확인

## 8) 의도된 실패 시나리오

### fail-closed 오류 예시
- `missing required env: SFEPS_DB_PASS`
- `SFEPS_DB_HOST must be localhost (local-only mode)`
- `RTSPS_TLS_CA is not readable`

이 경우는 보안 정책상 정상 동작입니다.

## 9) 별도 이슈

다음 로그는 현재 TLS 보안 작업과 별개 이슈입니다.
- `rc522 socket connect: No such file or directory`
- `rc522 socket connect: Permission denied`
- `Address already in use`
