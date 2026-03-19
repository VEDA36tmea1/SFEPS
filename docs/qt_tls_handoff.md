# SFEPS Qt Client TLS 전환 요청서

최종 갱신: 2026-03-19
대상: Qt Client 담당자

## 목적

- Qt 클라이언트에서 설정 1개로 `Plain(TCP)` / `TLS` 전환 지원
- 서버 dual-stack/TLS-only 전환 준비

## 현재 서버 상태 (확정)

Plain 포트:
- `5555` auth
- `5556` audio
- `5557` alert
- `5558` position
- `5559` video catalog

TLS 포트 기본값:
- `6555` auth
- `6556` audio
- `6557` alert
- `6558` position
- `6559` video catalog

서버 모드 환경변수:
1. Plain only
   - `SFEPS_APP_TLS_ENABLE=0`
   - `SFEPS_APP_PLAINTEXT_ENABLE=1`
2. Dual-stack
   - `SFEPS_APP_TLS_ENABLE=1`
   - `SFEPS_APP_PLAINTEXT_ENABLE=1`
3. TLS only
   - `SFEPS_APP_TLS_ENABLE=1`
   - `SFEPS_APP_PLAINTEXT_ENABLE=0`

## Qt 구현 요청 사항 (우선순위: auth/audio/alert)

1. `AuthManager`, `FraudManager`, `VoiceManager`에서 모드 플래그 기반 소켓 선택
2. Plain 모드:
   - 기존 `QTcpSocket` 유지
3. TLS 모드:
   - `QSslSocket` 사용
   - `connectToHostEncrypted()` 사용
4. TLS 검증:
   - `VerifyPeer` 필수
   - `ignoreSslErrors()` 금지
   - CA 파일 로드 + 신뢰체인 검증
   - SAN/hostname 불일치 시 연결 실패 처리
5. TLS 실패 시 자동 Plain fallback 금지
6. 데이터 포맷/프로토콜 문자열은 기존과 동일 유지

## 데이터 계약 (변경 없음)

1. Auth
   - 요청: `id:password` (UTF-8)
   - 응답: `PASS` 또는 `FAIL`
2. Alert
   - 수신 예시:
     - `FRAUD|...`
     - `TEST|LOGIN_OK|...`
     - `AUTH|FORCE_LOGOUT|REASON=POSITION_UNAUTHENTICATED|PROTO=...`
3. Audio
   - 송신: RAW PCM `16kHz`, `mono`, `Int16(S16_LE)`

## Qt 환경변수 제안

1. `SFEPS_CLIENT_TLS_ENABLE=0|1`
2. `SFEPS_CLIENT_CA_FILE=/path/to/ca.crt`
3. `SFEPS_CLIENT_TLS_SERVER_NAME=<cert SAN과 일치하는 host>`
4. 기존 호스트 변수 유지
   - `AUTH_SERVER_HOST`
   - `FRAUD_SERVER_HOST`
   - `AUDIO_SERVER_HOST`
5. 포트 자동 선택
   - Plain: `5555/5556/5557`
   - TLS: `6555/6556/6557`

## 완료 조건 (DoD)

1. `SFEPS_CLIENT_TLS_ENABLE=0`에서 기존 동작 동일
2. `SFEPS_CLIENT_TLS_ENABLE=1`에서 auth/audio/alert TLS 연결 성공
3. 잘못된 CA/hostname일 때 연결 차단 + 명확한 에러 로그
4. 서버 dual-stack에서 Plain/TLS 모두 접속 확인
5. 서버 TLS-only에서 Qt TLS만 성공, Plain 실패 확인

## 테스트 매트릭스

1. 서버 dual-stack + Qt Plain => 성공
2. 서버 dual-stack + Qt TLS => 성공
3. 서버 TLS-only + Qt Plain => 실패(정상)
4. 서버 TLS-only + Qt TLS => 성공
5. 서버 인증서 불일치 + Qt TLS => 실패(정상)

## 참고 파일

Qt:
- `client/src/authmanager.cpp`
- `client/src/fraudmanager.cpp`
- `client/src/voicemanager.cpp`

Server:
- `server/src/security_runtime.cpp`
- `server/src/services/transport_utils.cpp`
- `server/run_server.sh`
