# SFEPS Qt Client TLS 전환 요청서

작성일: 2026-03-03
대상: Qt Client 담당자

## 목적

- Qt 클라이언트에서 설정 1개로 `Plain(TCP)` / `TLS` 전환 가능하게 구현
- 서버는 이미 dual-stack 지원 중이므로, 클라이언트가 맞춰서 점진 전환 가능하도록 정렬

## 현재 서버 상태 (확정)

- Plain 포트
  - `5555` (auth)
  - `5556` (audio)
  - `5557` (alert)
- TLS 포트 (기본)
  - `6555` (auth)
  - `6556` (audio)
  - `6557` (alert)

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

## Qt 구현 요청 사항

1. `AuthManager`, `FraudManager`, `VoiceManager`에서 모드 플래그 기반 소켓 선택 지원
2. Plain 모드
   - 기존 `QTcpSocket` 유지
3. TLS 모드
   - `QSslSocket` 사용
   - `connectToHostEncrypted()` 사용
4. TLS 검증
   - 서버 인증서 검증 필수 (`VerifyPeer`)
   - `ignoreSslErrors()` 사용 금지
   - CA 파일 로드 후 신뢰체인 검증
   - 서버명(SAN/hostname) 검증 실패 시 연결 실패 처리
5. TLS 모드 실패 시 자동 Plain fallback 금지
6. 데이터 포맷은 기존과 100% 동일 유지

## 데이터 계약 (변경 없음)

1. Auth
   - 요청: `id:password` (UTF-8)
   - 응답: `PASS` 또는 `FAIL`
2. Alert
   - 수신: `FRAUD|cardId|ageGroup|gateId|estAge\n`
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
5. 포트는 모드별 자동 선택
   - Plain: `5555/5556/5557`
   - TLS: `6555/6556/6557`

## 완료 조건 (DoD)

1. `SFEPS_CLIENT_TLS_ENABLE=0`에서 기존 동작 동일
2. `SFEPS_CLIENT_TLS_ENABLE=1`에서 auth/audio/alert TLS 연결 성공
3. 잘못된 CA 또는 hostname일 때 연결 차단 + 명확한 에러 로그 출력
4. 서버 dual-stack에서 Plain/TLS 모두 접속 확인
5. 서버 TLS-only에서 Qt TLS 접속만 성공, Plain 접속 실패 확인

## 테스트 매트릭스

1. 서버 dual-stack + Qt Plain => 성공
2. 서버 dual-stack + Qt TLS => 성공
3. 서버 TLS-only + Qt Plain => 실패 (정상)
4. 서버 TLS-only + Qt TLS => 성공
5. 서버 인증서 불일치 + Qt TLS => 실패 (정상)

## 구현 시 참고 파일 (Qt)

- `client/src/authmanager.cpp`
- `client/src/fraudmanager.cpp`
- `client/src/voicemanager.cpp`

## 구현 시 참고 파일 (Server)

- `server/src/main.cpp`
- `server/src/tls_server.cpp`
- `server/run_server.sh`
