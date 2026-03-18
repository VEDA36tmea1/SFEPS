# Qt 전달용: 서버 다운/재기동 통합 강제 로그아웃 동작 안내

## 1) 이번 배포 기준 (필수 동작)
- 서버와 Qt를 다음 2축으로 연동합니다.
  1. **서버 다운으로 연결이 끊기면 Qt가 5초 후 자동 로그아웃**
  2. 서버 재기동 후 `Position` 접속이 `unauthenticated`로 거절되면, 서버가 해당 IP에
     `AUTH|FORCE_LOGOUT|...` 이벤트를 전송

- 인증 세션은 기존처럼 **IP 기반**입니다.

## 2) 서버 동작 (이번 배포에서 반영)

### 2-1. IP 타깃 Alert 전송
- 서버 Alert 레이어에 IP 타깃 전송 API 추가:
  - `send_alert_to_ip_clients(const std::string& ip, const std::string& msg)`
- 기존 브로드캐스트 API(`send_alert_to_clients`)는 그대로 유지됩니다.

### 2-2. Position `unauthenticated` 거절 시 강제 로그아웃 이벤트
- Plain/TLS 모두 아래 형식으로 이벤트 전송:
  - `AUTH|FORCE_LOGOUT|REASON=POSITION_UNAUTHENTICATED|PROTO=PLAIN\n`
  - `AUTH|FORCE_LOGOUT|REASON=POSITION_UNAUTHENTICATED|PROTO=TLS\n`
- 전송 폭주 방지:
  - 동일 IP에 대해 **5초 쿨다운** 적용

## 3) Qt 필수 구현 사항

### 3-1. 서버 다운 감지 기반 5초 자동 로그아웃
- 조건: `Alert` 또는 `Position` 소켓 중 **하나라도** `disconnected`
- 동작:
  1. 5초 타이머 시작
  2. 5초 내 복구되면 타이머 취소
  3. 타이머 만료 시 로그인 화면으로 복귀 + 세션 초기화

### 3-2. 서버 강제 로그아웃 이벤트 연동
- `Alert` 수신 메시지에서 아래 패턴 수신 시 즉시 강제 로그아웃:
  - `AUTH|FORCE_LOGOUT|...`
- 즉시 로그아웃 시에도 중복 처리 방지를 위해 가드 플래그 사용 권장:
  - 예: `isForceLogoutInProgress`

### 3-3. 중복 실행 방지
- 타이머 기반 로그아웃과 이벤트 기반 로그아웃이 동시에 들어올 수 있으므로,
  공통 강제 로그아웃 진입점 1개로 통합
- 이미 로그아웃 진행 중이면 이후 트리거는 무시

## 4) 권장 UI/상태 처리
- 강제 로그아웃 시 메인 화면 닫기, 로그인 화면 표시
- `AuthManager.currentUserId` 초기화
- 사용자 안내 문구 예시:
  - `서버 연결이 끊겨 자동 로그아웃되었습니다`
  - `인증이 만료되어 다시 로그인해 주세요`

## 5) 검증 시나리오
1. 서버 다운 시나리오
- Qt 실행 중 서버 중지
- 5초 후 로그인 화면 복귀 확인

2. 서버 재기동 + unauthenticated 시나리오
- 로그인 없이 Position 재접속
- 서버 로그의 `unauthenticated` 및 `AUTH|FORCE_LOGOUT` 전송 로그 확인
- Qt 즉시 로그아웃 확인

3. 타깃 전송 검증
- 서로 다른 IP 2대 중 한쪽만 `unauthenticated` 유도
- 해당 IP 클라이언트만 로그아웃되는지 확인

4. 쿨다운 검증
- 동일 IP 재시도 루프에서 5초 내 이벤트 재전송 억제 확인

5. 회귀 검증
- 기존 `FRAUD|...`, `TEST|LOGIN_OK|...` 수신/표시 동작 유지 확인

## 6) 서버 로그 키워드
- `Position ... rejected: unauthenticated ip=...`
- `force logout event dispatched`
- `force logout event skipped (cooldown)`
- `Target dispatch start`
- `Target dispatch done`
