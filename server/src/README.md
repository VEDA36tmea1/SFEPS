# src

`server/src`는 SFEPS 서버의 실행 엔트리포인트와 주요 내부 계층을 담고 있습니다. 이 디렉토리에서 가장 중요한 파일은 `main.cpp`이며, 실제 서버 기동 순서와 각 서브시스템 연결은 여기서 결정됩니다.

## `main.cpp` 역할

`main.cpp`는 개별 기능을 직접 구현하기보다, 서버 전체를 조립하고 실행 순서를 관리하는 오케스트레이션 레이어입니다.

주요 책임:
- 런타임 설정과 보안 설정 로드
- DB/Auth 사전 점검
- 로그, 분석기, ESP, 정리 워커 초기화
- Auth, Audio, Alert, Position, Video Catalog 서비스 스레드 시작
- RFID 모니터와 RTSP Recorder 실행
- 종료 신호 수신 시 전체 스레드와 자원 정리

## `main.cpp` 실행 흐름

1. `RuntimeConfig` 로드
   - DB 접속 정보와 필수 환경변수를 읽습니다.
2. `SecurityRuntimeOptions` 로드 및 검증
   - allowlist, 포트, TLS, ESP, 타임아웃, 이미지 보관 정책을 검증합니다.
3. Auth DB 프로브
   - `Authenticator`로 최소 연결 점검을 수행해 fail-closed 여부를 결정합니다.
4. 런타임 보안 로그 출력
   - allowlist, plain/TLS 모드, ESP 설정, 각종 제한값을 시작 로그에 남깁니다.
5. 런타임 디렉토리 준비
   - 녹화 영상과 fraud/pending 이미지 디렉토리를 생성합니다.
6. `DBLogger` 시작
   - 로그인/녹화 로그 기록과 주기적 DB 정리 요청을 처리합니다.
7. `AnalyticsProcessor` 시작
   - RTSP 메타데이터와 RFID 이벤트를 받아 fraud 판단 상태를 유지합니다.
8. callback 연결
   - RFID 매칭 시 이미지 스냅샷
   - outline 판정 시 fraud 이미지 finalize + Alert 이미지 URL 전송
   - fraud 위치 갱신 시 ESP 추적 위치 전송
9. `EspManager` 시작
   - 설정이 켜져 있으면 ESP TCP 서버를 기동합니다.
10. 백그라운드 워커 시작
   - 영상 파일 정리
   - pending/fraud 이미지 정리
   - DB cleanup 요청 스레드
11. 서비스 스레드 시작
   - Auth
   - Audio
   - Alert
   - Position
   - Video Catalog
12. RFID monitor 시작
   - `/tmp/rc522_events.sock`를 감시합니다.
13. RTSP recorder 실행
   - 메인 스레드에서 직접 녹화 루프를 돌며 메타데이터를 분석기로 전달합니다.
14. 종료 처리
   - `g_running=false`
   - Alert/ESP 연결 정리
   - 모든 워커/서비스 스레드 join
   - `analytics.stop()` 호출 후 종료 로그 출력

## 주요 스레드 구성

- 메인 스레드
  - `RTSPRecorder::run()` 실행
- 서비스 스레드
  - Auth
  - Audio
  - Alert
  - Position
  - Video Catalog
- 운영 스레드
  - 파일 정리
  - pending 이미지 정리
  - fraud 이미지 정리
  - DB cleanup
  - RFID monitor

## 주요 연결 포인트

### `core/`
- 설정 로드, 네트워크 공통 유틸, 운영성 작업, 서비스 부트스트랩을 제공합니다.

### `services/`
- 서비스 구현 공통 transport/helper 계층을 제공합니다.

### `modules/`
- 실제 인증, 스트리밍, 알림, 음성, UI, ESP 기능 구현이 들어 있습니다.

## `g_running` 의미

- 서버 전체 종료 플래그입니다.
- `SIGINT`, `SIGTERM`을 받으면 `signal_handler()`가 `g_running=false`로 바꿉니다.
- 대부분의 루프와 워커는 이 값을 보고 빠져나오도록 작성되어 있습니다.

## 읽는 순서 추천

1. `main.cpp`
   - 전체 조립 순서 파악
2. `core/README.md`
   - 설정/네트워크/운영 기반 이해
3. `modules/README.md`
   - 실제 기능 모듈 흐름 이해
4. `services/README.md`
   - 공통 transport/helper 계층 확인
