# auth

이 폴더는 사용자 로그인 인증과 인증 결과에 따른 세션성 상태 전파를 담당합니다.

## 파일

### `auth.cpp`
- `Authenticator`를 구현합니다.
- `users` 테이블을 대상으로 `id`, `password`를 prepared statement로 조회합니다.
- DB 연결과 statement 재사용을 담당하며, `authenticate()`는 단순히 "존재 여부"만 반환합니다.

### `auth_service.cpp`
- Auth TCP/TLS 포트 서버 구현입니다.
- 요청 형식:
  - `id:password`
- 응답 형식:
  - `PASS`
  - `FAIL`
- 주요 동작:
  - allowlist 검증 후 클라이언트 수락
  - `SFEPS_AUTH_MAX_BYTES` 초과 payload 거부
  - `user|ip` 기준 실패 횟수 추적
  - 5회 실패 시 30초 lock
  - 성공 시 인증 IP 등록과 `TEST|LOGIN_OK|<user>` Alert 전송
  - 로그인 시도 결과를 `DBLogger`로 비동기 기록

## 설계 포인트

- 인증 상태는 이후 `ui/position_service.cpp`에서 재사용됩니다.
- 별도의 세션 토큰 없이 IP 기반 인증 상태를 공유하므로, Position 서비스와 강하게 연결되어 있습니다.
- Auth logger DB 연결이 실패해도 인증 서비스 자체는 계속 동작하도록 설계되어 있습니다.
