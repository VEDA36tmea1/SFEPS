# Qt 전달용: 종료/재실행 시 인증 재검증 동작 안내

## 1) 이번 서버 배포 기준
- **Qt 필수 변경 없음**
- 서버에서 Position 채널 종료를 감지하면 인증 IP 세션을 유예 후 해제합니다.
- 환경변수: `SFEPS_AUTH_DEAUTH_GRACE_MS` (기본 `3000ms`)

동작 요약:
1. 로그인 성공 -> 서버가 해당 IP를 인증 세션으로 등록
2. Position 연결 종료 -> 서버가 `deauth scheduled` 로그와 함께 해제 예약
3. 유예 시간 내 재연결 없음 -> 서버가 인증 세션 해제 (`auth session released`)
4. 이후 Qt 재실행 시 로그인 없이 Position 접속하면 `unauthenticated`로 거절

## 2) Qt 2차 권장 변경 (선택)
아래는 필수는 아니지만 운영 품질을 높입니다.

1. 앱 종료 시 `Auth` 채널에 `LOGOUT|<userId>\n` 전송
- 서버가 즉시 세션 정리 가능
- 유예시간 대기 없이 재검증 일관성 확보

2. Position 연결 시작 시점을 `loginSuccess` 이후로 이동
- 서버 재기동 구간에서 불필요한 `unauthenticated` 재접속/로그 감소

3. 재연결 백오프 적용
- 현재 고정 재시도(예: 1초) 대신 지수 백오프 적용 권장
- 서버 다운/네트워크 불안정 시 connect 폭주 감소

## 3) 검증 시나리오
1. 정상 로그인 후 Position 수신 확인
2. Qt 종료 후 `SFEPS_AUTH_DEAUTH_GRACE_MS` 경과 뒤 재실행
- 로그인 없이 Position 접속 시 거절되어야 함
3. Qt 종료 직후(유예 내) 재실행
- 재연결 시 예약 해제가 취소되어 연결이 유지될 수 있음
4. 서버 재시작 후 로그인 없이 접속
- 기존과 동일하게 거절

## 4) 서버 로그 키워드
- `deauth scheduled`
- `deauth canceled (reconnect)`
- `auth session released`
- `Position ... rejected: unauthenticated ip=...`
