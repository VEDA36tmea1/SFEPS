# SFEPS Server

최종 갱신: 2026-03-27

이 문서는 `server`가 실제로 어떤 기능을 담당하고, 코드가 어떤 순서로 실행되며, 데이터가 어떤 경로로 흘러가는지를 기준으로 다시 정리한 README입니다. 요약이 아니라 "실행 흐름 + 데이터 흐름" 중심으로 읽을 수 있게 구성했습니다.

## 1. 서버가 하는 일

이 서버는 하나의 프로세스 안에서 아래 역할을 함께 수행합니다.

- RTSP 카메라 영상 녹화
- ONVIF 메타데이터(XML) 분석
- RFID 이벤트 수신 및 객체 매칭
- 부정 사용(fraud) 판단
- 로그인 인증(Auth)
- 음성 수신(Audio)
- 알림 브로드캐스트(Alert)
- 객체 위치 스트리밍(Position)
- 녹화 목록/재생 URL 제공(Video Catalog)

핵심은 단순한 "API 서버"가 아니라, 카메라/DB/RFID/앱 클라이언트를 한 번에 연결하는 실시간 이벤트 허브에 가깝다는 점입니다.

## 2. 전체 구조

`server/src`는 대략 아래 4계층으로 나뉩니다.

- `main.cpp`
  - 전체 프로세스 조립, 실행 순서 제어, 스레드 시작/종료
- `core/`
  - 설정 로드, TLS/TCP 공통 네트워크, DB 로깅, 정리 워커
- `services/`
  - Auth/Alert/Audio/Position/Video Catalog 서비스가 공통으로 쓰는 transport/helper
- `modules/`
  - 실제 도메인 로직
  - `auth/`, `alerts/`, `voice/`, `ui/`, `streaming/`

읽는 시작점은 항상 `server/src/main.cpp`입니다.

## 3. 기동 순서

실제 서버 시작 순서는 `server/src/main.cpp` 기준으로 아래와 같습니다.

1. `RuntimeConfig` 로드
   - `SFEPS_DB_HOST`, `SFEPS_DB_USER`, `SFEPS_DB_PASS`, `SFEPS_DB_NAME_ANALYTICS`를 읽습니다.
   - DB 호스트는 `localhost`만 허용합니다.

2. `SecurityRuntimeOptions` 로드 및 검증
   - allowlist, 최대 payload, 포트, TLS, retention 값을 읽습니다.
   - Auth/Audio/Alert allowlist가 비어 있으면 fail-closed로 종료합니다.
   - TLS 사용 시 cert/key 가독성과 포트 충돌도 검사합니다.

3. Auth DB 사전 점검
   - `Authenticator`로 DB 연결을 한 번 확인합니다.
   - 여기서 실패하면 서버 전체가 뜨지 않습니다.

4. 런타임 디렉토리 준비
   - `/home/iam/SFEPS/videos`
   - `/home/iam/SFEPS/event_images/pending`
   - `/home/iam/SFEPS/event_images/fraud`
   - `/home/iam/SFEPS/event_images/failed`

5. `DBLogger` 시작
   - 로그인 로그, 녹화 파일 로그, 오래된 DB 레코드 정리를 비동기로 처리합니다.

6. `AnalyticsProcessor` 시작
   - Recorder와 RFID 모니터가 넣는 데이터를 받아 객체 상태와 fraud 상태를 관리합니다.

7. Analytics callback 연결
   - RFID 매칭 성공 시 이미지 캡처 요청
   - outline 판정 완료 시 fraud 이미지 finalize + `IMG_REF` Alert 전송

8. 백그라운드 워커 시작
   - 영상 파일 retention / 용량 정리
   - pending/fraud 이미지 정리
   - DB cleanup 요청 스레드

9. 앱 서비스 스레드 시작
   - Auth
   - Audio
   - Alert
   - Position
   - Video Catalog

10. `RfidMonitor` 시작
   - `/tmp/rc522_events.sock`를 감시합니다.

11. 메인 스레드에서 `RTSPRecorder::run()`
   - RTSP 연결 유지
   - MP4 세그먼트 저장
   - 메타데이터 XML 추출 후 Analytics로 전달

12. 종료 시
   - `g_running=false`
   - Alert 연결 정리
   - 각 스레드 join
   - `analytics.stop()`

즉, 이 서버는 "서비스 몇 개를 등록하고 끝"이 아니라, `Recorder`와 `Analytics`가 중심축이 되고 나머지 서비스가 그 상태를 구독/전달하는 구조입니다.

## 4. 가장 중요한 데이터 흐름

### 4.1 카메라 메타데이터 -> 객체 상태 -> Alert/Position

관련 파일:

- `server/src/modules/streaming/recorder.cpp`
- `server/src/modules/streaming/analytics.cpp`
- `server/src/modules/alerts/alert.cpp`
- `server/src/modules/ui/position_service.cpp`

흐름은 아래와 같습니다.

1. `RTSPRecorder`가 `SFEPS_RTSP_URL`로 RTSP에 붙습니다.
2. 비디오 스트림은 1분 단위 MP4 파일로 잘라 저장합니다.
3. 메타데이터 스트림은 XML 조각을 재조립해서 `AnalyticsProcessor::publishRaw()`로 넘깁니다.
4. `AnalyticsProcessor`는 XML에서
   - 사람 객체 bbox/중심점
   - 이벤트 rule 이름
   - 이벤트 object id
   - `UtcTime`
   를 읽습니다.
5. 최신 객체 위치는 `latest_objects`에 저장됩니다.
6. outline 이벤트가 오면 fraud 여부를 최종 계산합니다.
7. 결과는 동시에 여러 곳으로 퍼집니다.
   - Alert: `FRAUD|...`
   - Position: `getAllObjectSnapshots()`를 통해 주기적으로 방송
   - DB: fraud일 때만 `analytics_logs` INSERT

중요한 점:

- Position 서비스는 자체적으로 객체를 계산하지 않습니다.
- Position은 `AnalyticsProcessor`가 유지하는 최신 스냅샷을 읽어서 방송합니다.

### 4.2 RFID -> pending 객체 매칭 -> 이미지 캡처 -> fraud 이미지 확정

관련 파일:

- `server/src/modules/streaming/rfid_monitor.cpp`
- `server/src/modules/streaming/analytics.cpp`
- `server/src/modules/streaming/rfid_image_pipeline.cpp`
- `server/src/main.cpp`

이 흐름이 이 프로젝트에서 가장 중요한 도메인 로직입니다.

1. 카메라 메타데이터에서 `enterline` 이벤트가 들어오면 `AnalyticsProcessor`는 해당 object id를 pending 큐에 넣습니다.
   - 기본값은 `card_age_text="0"`
   - 기본 fraud 상태는 `true`

2. RFID 데몬이 `/tmp/rc522_events.sock`로 NDJSON 한 줄을 보내면 `RfidMonitor`가 읽습니다.
   - `id`
   - `text`
   - `timestamp`

3. `RfidMonitor`는 `text` 값을 `AnalyticsProcessor::onRfidRead()`로 넘깁니다.

4. `onRfidRead()`는 pending 큐의 가장 앞 객체 하나를 꺼내 카드 정보와 매칭합니다.
   - 여기서 RFID는 "가장 오래 기다리던 pending 객체"와 결합됩니다.
   - 매칭 성공 시 `matched_objects`로 이동합니다.

5. RFID 매칭 callback이 실행되면 `snapshot_rfid_image_for_object()`가 호출됩니다.
   - `/tmp/sfeps_camera_trigger.sock`로 캡처 요청 전송
   - 결과 이미지는 `event_images/pending/`에 저장되도록 요청

6. 나중에 같은 객체에 대해 `outline` 이벤트가 오면 fraud 여부를 최종 계산합니다.
   - 카드 연령과 카메라 추정 연령 bucket 비교
   - 미확정/미인식 상태도 fraud로 취급

7. outline 결정 callback이 실행되면 `finalize_outline_image_for_object()`가 호출됩니다.
   - fraud면 `event_images/fraud/`로 이동
   - fraud가 아니면 삭제, 실패 시 `failed/`로 이동

8. fraud 이미지가 최종 보관되면 `main.cpp`가
   - `SFEPS_FRAUD_IMAGE_HTTP_BASE_URL`
   - 이미지 파일명
   을 합쳐 `IMG_REF|...` Alert를 추가 전송합니다.

즉, RFID 이미지는 "RFID 읽힌 순간"이 아니라 "RFID와 pending 객체가 매칭된 순간에 캡처 요청"되고, "outline 판정이 끝난 뒤" 최종 보관 여부가 결정됩니다.

### 4.3 로그인(Auth) -> 인증 IP 등록 -> Position 접근 허용

관련 파일:

- `server/src/modules/auth/auth.cpp`
- `server/src/modules/auth/auth_service.cpp`
- `server/src/services/service_shared.cpp`
- `server/src/modules/ui/position_service.cpp`

흐름은 아래와 같습니다.

1. Auth 클라이언트가 `5555` 또는 TLS 포트로 접속합니다.
2. 요청 형식은 `id:password`입니다.
3. `Authenticator`가 `users` 테이블을 prepared statement로 조회합니다.
4. 성공하면
   - 응답 `PASS`
   - `mark_ip_authenticated(ip)`
   - Alert에 `TEST|LOGIN_OK|<user>`
   - `login_logs` 비동기 적재
5. 실패하면
   - 응답 `FAIL`
   - 실패 횟수 증가
   - `user|ip` 기준 5회 실패 시 30초 lock

핵심 설계:

- 별도 세션 토큰이 없습니다.
- Position 서비스 접근 권한은 "로그인 성공한 IP인지"로 판단합니다.
- 따라서 Auth와 Position은 IP 기반 세션 상태를 공유합니다.

### 4.4 Position 구독 -> 객체 위치 스트림 -> 인증 해제 처리

관련 파일:

- `server/src/modules/ui/position_service.cpp`
- `server/src/services/service_shared.cpp`
- `server/src/modules/alerts/alert.cpp`

흐름은 아래와 같습니다.

1. Position 클라이언트가 `5558` 또는 TLS 포트로 접속합니다.
2. 서버는 먼저 `is_ip_authenticated(ip)`를 검사합니다.
3. 인증되지 않은 IP면
   - 연결 거부
   - 같은 IP Alert 구독자에게 `AUTH|FORCE_LOGOUT|...` 전송

4. 인증된 클라이언트는 아래 명령을 보냅니다.
   - `SUB_POS|<object_id>`
   - `UNSUB_POS|<object_id>`

5. 서버는 `AnalyticsProcessor::getAllObjectSnapshots()`를 주기적으로 읽어
   - `OBJ_POS|...`
   - `OBJ_END|...`
   를 연결된 Position 클라이언트들에게 전송합니다.

6. Position 연결이 끊기면 즉시 인증을 지우지 않고 grace period를 둡니다.
   - `SFEPS_AUTH_DEAUTH_GRACE_MS`
   - 같은 IP로 재연결하면 예약된 deauth를 취소
   - 끝까지 재연결이 없으면 `unmark_ip_authenticated(ip)`

즉, Position은 실시간 스트림이면서 동시에 "앱 세션 유지 상태"를 담당하는 서비스이기도 합니다.

### 4.5 Video Catalog -> recordings 레지스트리 -> HTTP 재생 URL

관련 파일:

- `server/src/modules/ui/video_catalog_service.cpp`
- `server/src/modules/ui/video_catalog_events.cpp`
- `server/src/core/ops/log.cpp`
- `server/src/core/ops/cleanup.cpp`

흐름은 아래와 같습니다.

1. 서버 시작 시 `recordings` 테이블을 읽어 초기 카탈로그를 메모리에 적재합니다.
2. 클라이언트가 Video Catalog 포트에 붙으면
   - `REC_SNAPSHOT_BEGIN`
   - `REC|id|created_at`
   - `REC_SNAPSHOT_END`
   - `REC_STORAGE`
   순서로 초기 스냅샷을 받습니다.

3. 녹화 세그먼트가 닫힐 때 `DBLogger::enqueueRecording()`이 실행됩니다.
4. `DBLogger` 워커는 `recordings` 테이블 INSERT 후, 방금 저장된 레코드를 다시 읽어 `publish_video_catalog_record_added()`를 호출합니다.
5. Video Catalog 서비스는 메모리 이벤트 히스토리를 보고 각 클라이언트에게
   - `REC_ADD|...`
   - `REC_DEL|...`
   을 push 합니다.

6. 클라이언트가 `PLAY_REC|<id>`를 보내면 서버는 MP4 자체를 보내지 않고 `PLAY_URL|...`만 응답합니다.
7. 실제 영상 재생은 `SFEPS_VIDEO_HTTP_BASE_URL` 뒤의 정적 파일 URL로 처리합니다.

중요한 점:

- Video Catalog는 "목록/이벤트/상태 브로드캐스트"용 TCP 서비스입니다.
- 실제 MP4 바이트 전송은 별도의 HTTP 정적 서빙이 담당합니다.
- cleanup 워커가 파일을 삭제하면 `REC_DEL` 이벤트도 같이 발생합니다.
- 영상 정리 정책은 기본적으로 `SFEPS_VIDEO_MAX_STORAGE_BYTES=5GB`를 넘으면
  가장 오래된 파일부터 지워 `SFEPS_VIDEO_STORAGE_RESUME_BYTES=3GB` 이하로 내리는 방식입니다.

### 4.6 Audio -> 링 버퍼 -> 로컬 재생

관련 파일:

- `server/src/modules/voice/audio_service.cpp`

흐름은 단순합니다.

1. Audio 클라이언트가 접속합니다.
2. 수신 바이트를 `AudioRingBuffer`에 밀어 넣습니다.
3. `AudioPlayback` 스레드가 이를 소비해 로컬 장치로 재생합니다.

부가 동작:

- RFID 태그가 읽힐 때 입력 오디오 클라이언트가 없으면 로컬 비프음을 재생합니다.

## 5. 모듈별 책임 정리

### `core/config`

- 환경변수 로드
- 보안/포트/TLS/fail-closed 검증

### `core/network`

- TCP listen socket 생성
- TLS 서버 초기화/handshake
- allowlist 검사

### `core/ops`

- `DBLogger`
- 영상/이미지 retention cleanup

### `services`

- plain/TLS 공통 transport
- 인증된 IP 상태 공유
- Position/Video Catalog 포맷 헬퍼

### `modules/auth`

- DB 인증
- 로그인 시도 제한
- 로그인 성공 시 인증 IP 등록

### `modules/alerts`

- Alert 구독 클라이언트 유지
- 서버 전역 이벤트 브로드캐스트

### `modules/streaming`

- RTSP 녹화
- XML 메타데이터 해석
- RFID 매칭
- fraud 판단
- fraud 이미지 파이프라인

### `modules/ui`

- Position 스트림
- Video Catalog

### `modules/voice`

- 오디오 수신 및 재생 버퍼링

## 6. 서버가 쓰는 주요 상태 저장소

### 메모리 상태

- 인증된 IP 목록
  - Auth 성공 시 등록
  - Position 연결 종료 후 grace period 경과 시 제거

- 최신 객체 위치
  - Analytics가 유지
  - Position이 읽음

- pending 객체 큐
  - enterline 이벤트로 생성
  - RFID와 매칭 대기

- matched 객체
  - RFID가 붙은 뒤 outline 대기 상태

- Alert 구독 클라이언트 목록

- Video Catalog 레지스트리/이벤트 히스토리

### DB 테이블

- `users`
  - 로그인 인증
- `login_logs`
  - 로그인 성공/실패 이력
- `recordings`
  - 저장된 MP4 세그먼트 목록
- `analytics_logs`
  - fraud 판정 결과 저장

주의:

- 제공된 SQL 파일 `server/sql/ccgbd_auth_min_tables.sql`은 `users`, `login_logs` 중심입니다.
- `recordings`, `analytics_logs`는 코드상 필수로 사용되므로 운영 DB에는 별도로 준비되어 있어야 합니다.

## 7. 포트와 프로토콜

### Plain TCP 기본값

| 포트 | 서비스 | 역할 |
|---|---|---|
| 5555 | Auth | 로그인 |
| 5556 | Audio | 오디오 입력 |
| 5557 | Alert | 서버 푸시 구독 |
| 5558 | Position | 위치 스트림 |
| 5559 | Video Catalog | 녹화 목록/재생 요청 |

### TLS 기본값

| 포트 | 서비스 |
|---|---|
| 6555 | Auth TLS |
| 6556 | Audio TLS |
| 6557 | Alert TLS |
| 6558 | Position TLS |
| 6559 | Video Catalog TLS |

### 주요 메시지

Auth:

- 요청: `id:password`
- 응답: `PASS`, `FAIL`

Alert:

- `TEST|LOGIN_OK|<user>`
- `FRAUD|<object_id>|<card_age_text>|<age>|<Y|N>|L=...|T=...|R=...|B=...|X=...|Y=...|TAG=...`
- `IMG_REF|OBJECT_ID=...|URL=...|TAG=...|NAME=...`
- `AUTH|FORCE_LOGOUT|REASON=POSITION_UNAUTHENTICATED|PROTO=...`

Position:

- `SUB_POS|<object_id>`
- `UNSUB_POS|<object_id>`
- `OBJ_POS|...`
- `OBJ_END|<object_id>|REASON=...`

Video Catalog:

- `REC_SNAPSHOT_BEGIN|TOTAL=n`
- `REC|id|created_at`
- `REC_SNAPSHOT_END|TOTAL=n`
- `REC_ADD|id|created_at`
- `REC_DEL|id`
- `REC_STORAGE|USED_BYTES=n|CAP_BYTES=n`
- `SYS_STATUS|CPU_TEMP_C=...|CPU_USAGE_PCT=...`
- `PLAY_REC|<id>`
- `PLAY_URL|<id>|<created_at>|<url>`
- `PLAY_ERR|...`

참고:

- `Video Catalog` plain/TLS 포트는 환경변수로 변경 가능합니다.
- Auth/Audio/Alert/Position plain 포트는 코드상 `5555~5558`로 고정되어 있습니다.

## 8. 필수 환경변수

최소 기동 조건은 아래입니다.

```bash
export SFEPS_DB_HOST=localhost
export SFEPS_DB_USER=pi
export SFEPS_DB_PASS='***'
export SFEPS_DB_NAME_ANALYTICS=CCgbd

export SFEPS_AUTH_ALLOW_IPS="192.168.0.10"
export SFEPS_AUDIO_ALLOW_IPS="192.168.0.11"
export SFEPS_ALERT_ALLOW_IPS="192.168.0.10,192.168.0.12"
export SFEPS_VIDEO_MAX_STORAGE_BYTES=5368709120
export SFEPS_VIDEO_STORAGE_RESUME_BYTES=3221225472
```

정책 요약:

- DB는 로컬 호스트만 허용
- Auth/Audio/Alert allowlist 미설정 시 fail-closed
- Position/Video Catalog는 현재 `SFEPS_ALERT_ALLOW_IPS` 정책을 공유
- TLS 활성 시 cert/key 필수

## 9. 실행 방법

```bash
cd /home/iam/SFEPS/server
cmake -S . -B build
cmake --build build -j"$(nproc)"
./run_server.sh
```

`run_server.sh`는 아래 순서로 환경 파일을 자동 로드합니다.

1. `SFEPS_ENV_FILE`
2. `.env.local`
3. `.env`

## 10. 운영 시 알아둘 점

- Auth, Analytics, DBLogger 초기 연결은 비교적 fail-closed에 가깝게 동작합니다.
- Auth logger는 별도 연결 실패 시 경고만 남기고 인증 서비스 자체는 계속 동작합니다.
- Video Catalog는 시작 시 초기 카탈로그 로드가 실패하면 해당 서비스만 비활성화될 수 있습니다.
- `analytics_logs`에는 fraud 결과만 저장됩니다. 정상 판정은 DB에 남지 않습니다.
- Position 인증은 IP 기반이라 NAT/공유 단말 환경에서는 주의가 필요합니다.
- cleanup 워커가 MP4를 삭제하면 Video Catalog에서도 `REC_DEL` 이벤트가 발생합니다.

## 11. 코드 읽기 추천 순서

1. `server/src/main.cpp`
2. `server/src/core/config/runtime_config.cpp`
3. `server/src/core/config/security_runtime.cpp`
4. `server/src/modules/streaming/recorder.cpp`
5. `server/src/modules/streaming/analytics.cpp`
6. `server/src/modules/streaming/rfid_monitor.cpp`
7. `server/src/modules/streaming/rfid_image_pipeline.cpp`
8. `server/src/modules/auth/auth_service.cpp`
9. `server/src/modules/ui/position_service.cpp`
10. `server/src/modules/ui/video_catalog_service.cpp`

## 12. 한 줄 요약

이 서버는 "카메라 메타데이터와 RFID를 합쳐 객체별 fraud를 판정하고, 그 결과를 DB/Alert/Position/Video Catalog로 동시에 배포하는 실시간 멀티서비스 프로세스"입니다.
