# SFEPS Server

최종 갱신: 2026-03-25

SFEPS 서버는 아래 기능을 담당합니다.
- RTSP 녹화(1분 분할 MP4 저장)
- 로그인 인증(Auth)
- 음성 수신/재생(Audio)
- 알림 브로드캐스트(Alert)
- 객체 위치 스트리밍(Position)
- 녹화 영상 목록 구독/재생(Video Catalog)
- RFID 이벤트 수신 및 Analytics 매칭
- 선택 기능: ESP TCP 연동

## 런타임 필수 환경변수 (fail-closed)

`run_server.sh`와 서버 런타임 검증에서 아래 값이 없으면 기동이 거부됩니다.

```bash
# DB
export SFEPS_DB_HOST=localhost
export SFEPS_DB_USER=pi
export SFEPS_DB_PASS='***'
export SFEPS_DB_NAME_ANALYTICS=CCgbd

# allowlist (필수)
export SFEPS_AUTH_ALLOW_IPS="192.168.0.10"
export SFEPS_AUDIO_ALLOW_IPS="192.168.0.11"
export SFEPS_ALERT_ALLOW_IPS="192.168.0.10,192.168.0.12"

# 호환용(선택): 지정해도 런타임은 SFEPS_DB_NAME_ANALYTICS 단일 스키마 사용
# export SFEPS_DB_NAME_AUTH=CCgbd
```

정책 요약:
- `SFEPS_DB_HOST`는 `localhost`만 허용됩니다.
- Position/Video Catalog는 별도 allowlist 키가 없고, 현재 `SFEPS_ALERT_ALLOW_IPS` 정책을 공유합니다.
- 서버는 기본적으로 `SFEPS_RTSP_URL=rtsp://127.0.0.1:8554/cam1`를 사용합니다.

## 주요 선택형 환경변수

설정하지 않으면 아래 기본값이 적용됩니다.
(`run_server.sh` 실행 기준이며, 바이너리 직접 실행 시 일부 기본값은 런타임 코드 기본값을 따릅니다.)

```bash
# Recorder / Analytics
export SFEPS_RTSP_URL=rtsp://127.0.0.1:8554/cam1
export SFEPS_META_MAX_PACKET_BYTES=65536
export SFEPS_META_BAD_STREAK_LIMIT=20
export SFEPS_META_XML_BUFFER_MAX=1048576
export SFEPS_META_XML_DOC_MAX_BYTES=262144
export SFEPS_META_MAX_LINES_PER_BATCH=128
export SFEPS_ANALYTICS_QUEUE_MAX=200
export SFEPS_DROP_LOG_INTERVAL=100
export SFEPS_META_PENDING_MAX=2048
export SFEPS_META_PENDING_TTL_SEC=30
export SFEPS_META_ENTER_RULE=enterline
export SFEPS_META_OUTLINE_RULE=outline

# App service limits
export SFEPS_AUTH_MAX_BYTES=256
export SFEPS_AUDIO_MAX_BYTES=4194304
export SFEPS_ALERT_MAX_CLIENTS=64
export SFEPS_POSITION_MAX_CLIENTS=64
export SFEPS_VIDEO_MAX_CLIENTS=32
export SFEPS_SOCKET_READ_TIMEOUT_MS=5000
export SFEPS_POSITION_TICK_MS=100
export SFEPS_POSITION_MIN_SEND_MS=500
export SFEPS_POSITION_STALE_SEC=3
export SFEPS_AUTH_DEAUTH_GRACE_MS=3000

# App bind / TLS
export SFEPS_APP_BIND_IP=0.0.0.0
export SFEPS_APP_TLS_ENABLE=0
export SFEPS_APP_PLAINTEXT_ENABLE=1
export SFEPS_AUTH_TLS_PORT=6555
export SFEPS_AUDIO_TLS_PORT=6556
export SFEPS_ALERT_TLS_PORT=6557
export SFEPS_POSITION_TLS_PORT=6558
export SFEPS_VIDEO_CATALOG_PORT=5559
export SFEPS_VIDEO_CATALOG_TLS_PORT=6559
export SFEPS_APP_TLS_HANDSHAKE_TIMEOUT_MS=3000
# TLS 활성 시 필수
# export SFEPS_APP_TLS_CERT_FILE=/etc/sfeps/pki/server.crt
# export SFEPS_APP_TLS_KEY_FILE=/etc/sfeps/pki/server.key

# Video Catalog
export SFEPS_VIDEO_HTTP_BASE_URL=http://127.0.0.1:8080/videos
export SFEPS_FRAUD_IMAGE_HTTP_BASE_URL=http://127.0.0.1:8080/fraud-images
export SFEPS_VIDEO_RETENTION_SEC=86400
export SFEPS_PENDING_IMAGE_RETENTION_SEC=30
export SFEPS_FRAUD_IMAGE_RETENTION_SEC=86400

# ESP(선택)
export SFEPS_ESP_TCP_ENABLE=0
export SFEPS_ESP_TCP_BIND_IP=192.168.4.1
export SFEPS_ESP_TCP_PORT=5565
export SFEPS_ESP_TCP_MAX_CLIENTS=4
# export SFEPS_ESP_TCP_ALLOW_IPS="192.168.4.2"
# 개발용 TRACK_POS 테스트(5초 주기, 값 1 고정)
export SFEPS_ESP_TEST_TRACK_POS_ENABLE=0
export SFEPS_ESP_TEST_TRACK_POS_INTERVAL_SEC=5
export SFEPS_ESP_TEST_TRACK_POS_OBJECT_ID=ESP-TEST-01
```

## 핵심 포트

Plain:

| 포트 | 용도 | 프로토콜 |
|---|---|---|
| 5555 | 로그인 인증 | TCP |
| 5556 | 음성 수신 | TCP |
| 5557 | 알림 구독 | TCP |
| 5558 | 객체 위치 스트리밍 | TCP |
| 5559 | Video Catalog | TCP |

TLS:

| 포트 | 용도 | 프로토콜 |
|---|---|---|
| 6555 | 로그인 인증(TLS) | TLS/TCP |
| 6556 | 음성 수신(TLS) | TLS/TCP |
| 6557 | 알림 구독(TLS) | TLS/TCP |
| 6558 | 객체 위치 스트리밍(TLS) | TLS/TCP |
| 6559 | Video Catalog(TLS) | TLS/TCP |

## 앱 포트 TLS 모드

1. Plain only
```bash
export SFEPS_APP_TLS_ENABLE=0
export SFEPS_APP_PLAINTEXT_ENABLE=1
```

2. Dual-stack
```bash
export SFEPS_APP_TLS_ENABLE=1
export SFEPS_APP_PLAINTEXT_ENABLE=1
export SFEPS_APP_TLS_CERT_FILE=/etc/sfeps/pki/server.crt
export SFEPS_APP_TLS_KEY_FILE=/etc/sfeps/pki/server.key
```

3. TLS-only
```bash
export SFEPS_APP_TLS_ENABLE=1
export SFEPS_APP_PLAINTEXT_ENABLE=0
```

주의:
- `SFEPS_APP_TLS_ENABLE=1`이면 cert/key 경로가 필수이며 읽기 가능해야 합니다.
- TLS 포트(6555~6559)는 서로 중복되면 안 됩니다.
- Plain 활성 상태에서는 TLS 포트가 Plain 포트(5555~5559)와 충돌하면 기동 실패합니다.

## 빠른 실행

```bash
cd /home/iam/SFEPS/server
cmake -S . -B build
cmake --build build -j"$(nproc)"
./run_server.sh
```

`.env.local` 또는 `.env`가 있으면 `run_server.sh`가 자동 로드합니다.
명시적으로 파일을 지정하려면 `SFEPS_ENV_FILE`을 사용합니다.

```bash
SFEPS_ENV_FILE=/path/to/server.env ./run_server.sh
```

## 프로토콜 요약

Auth:
- 요청: `id:password`
- 응답: `PASS` 또는 `FAIL`

Alert:
- 로그인 성공 테스트: `TEST|LOGIN_OK|<user>\n`
- Fraud 알림:
  - `FRAUD|<object_id>|<card_age_text>|<age>|<Y|N>|L=<...>|T=<...>|R=<...>|B=<...>|X=<...>|Y=<...>|TAG=<...>\n`
- Fraud 이미지 참조:
  - `IMG_REF|OBJECT_ID=<object_id>|URL=<image_url>|TAG=<iso_time>|NAME=<filename>\n`
- 강제 로그아웃 이벤트:
  - `AUTH|FORCE_LOGOUT|REASON=POSITION_UNAUTHENTICATED|PROTO=PLAIN\n`
  - `AUTH|FORCE_LOGOUT|REASON=POSITION_UNAUTHENTICATED|PROTO=TLS\n`

Position:
- 구독: `SUB_POS|<object_id>\n`
- 구독해제: `UNSUB_POS|<object_id>\n`
- 서버 푸시:
  - `OBJ_POS|...|FRAUD=<Y|N>|TAG=<...>\n`
  - `OBJ_END|<object_id>|REASON=<...>\n`

ESP TCP:
- 서버 준비 신호: `ESP_READY|SERVER_ONLINE\n`
- 추적 이벤트 신호:
  - `TRACK_SWITCH|FROM=<old_object_id>|TO=<new_object_id>\n` (추적 대상 변경)
- 추적 시작 신호: `TRACK_START|<object_id>\n`
- 추적 위치 신호: `TRACK_POS|<object_id>|L=<...>|T=<...>|R=<...>|B=<...>|X=<...>|Y=<...>|CX=<...>|CY=<...>|W=<...>|H=<...>\n`
- 추적 종료 신호: `TRACK_END|<object_id>|REASON=<...>\n`

Video Catalog:
- 연결 직후 스냅샷:
  - `REC_SNAPSHOT_BEGIN|TOTAL=<n>`
  - `REC|<id>|<created_at>`
  - `REC_SNAPSHOT_END|TOTAL=<n>`
  - `REC_STORAGE|USED_BYTES=<n>|TOTAL_BYTES=<n>|AVAILABLE_BYTES=<n>|FILE_COUNT=<n>`
- 실시간 갱신:
  - `REC_ADD|<id>|<created_at>`
  - `REC_DEL|<id>`
  - `REC_STORAGE|USED_BYTES=<n>|TOTAL_BYTES=<n>|AVAILABLE_BYTES=<n>|FILE_COUNT=<n>`
- 상태 갱신:
  - `SYS_STATUS|CPU_TEMP_C=<float>|CPU_USAGE_PCT=<float>` (5초 주기 단독 전송)
- 재생 요청: `PLAY_REC|<id>\n`
- 재생 응답:
  - `PLAY_URL|<id>|<created_at>|<url>`
- 오류 응답:
  - `REC_ERR|<code>|<message>`
  - `PLAY_ERR|<code>|<message>`

## 운영 체크 명령

```bash
systemctl is-active mariadb
systemctl is-active mediamtx
systemctl is-active sfeps-server

journalctl -u sfeps-server -f
```

## RFID 부팅 자동화(systemd)

목표:
- 부팅 시 `sfeps-rfid-module.service`가 `rc522` 모듈 로드
- 서버 시작 전 `sfeps-rfid.service` 소켓(`/tmp/rc522_events.sock`) 준비
- 서버 종료 시 RFID 서비스 정리

수동 적용:

```bash
cd /home/iam/SFEPS

sudo cp hardware/Raspi-driver/RC522_RFID/systemd/sfeps-rfid.env.example /etc/default/sfeps-rfid
sudo vi /etc/default/sfeps-rfid

sudo cp hardware/Raspi-driver/RC522_RFID/systemd/sfeps-rfid-module.service /etc/systemd/system/
sudo cp hardware/Raspi-driver/RC522_RFID/systemd/sfeps-rfid.service /etc/systemd/system/

sudo cp server/systemd/sfeps-image-processing.service /etc/systemd/system/
sudo cp server/systemd/sfeps-server.service /etc/systemd/system/
sudo mkdir -p /etc/systemd/system/sfeps-server.service.d
sudo cp server/systemd/sfeps-server.service.d/rfid.conf /etc/systemd/system/sfeps-server.service.d/
sudo cp server/systemd/sfeps-server.service.d/image-processing.conf /etc/systemd/system/sfeps-server.service.d/

sudo systemctl daemon-reload
sudo systemctl enable --now sfeps-rfid-module.service
sudo systemctl enable --now sfeps-server.service
```

## 주요 경로

- 서버 실행: `/home/iam/SFEPS/server/run_server.sh`
- 서버 바이너리: `/home/iam/SFEPS/server/build/smart_server.bin`
- 영상 저장: `/home/iam/SFEPS/videos`
- 이벤트 이미지: `/home/iam/SFEPS/event_images`
- RFID 소켓: `/tmp/rc522_events.sock`

운영 참고:
- `IMG_REF` URL은 `SFEPS_FRAUD_IMAGE_HTTP_BASE_URL` 기반으로 생성됩니다.
- `PLAY_URL` URL은 `SFEPS_VIDEO_HTTP_BASE_URL` 기반으로 생성됩니다.
- 운영에서 `http://<host>:8080/videos/<filename>`가
  `/home/iam/SFEPS/videos/<filename>`로 매핑되도록 정적 파일 서빙 구성이 필요합니다.
- Qt seek/탐색을 위해 `/videos` 정적 서버는 HTTP Range 요청을 지원해야 합니다.
- 영상은 `SFEPS_VIDEO_RETENTION_SEC`(기본 86400초, 1일) 지난 파일부터 자동 삭제됩니다.
- 운영에서 `http://<host>:8080/fraud-images/<filename>`가
  `/home/iam/SFEPS/event_images/fraud/<filename>`로 매핑되도록 정적 파일 서빙 구성이 필요합니다.
- pending 이미지는 `SFEPS_PENDING_IMAGE_RETENTION_SEC`(기본 30초) 지난 파일부터 자동 삭제됩니다.
- fraud 이미지는 `SFEPS_FRAUD_IMAGE_RETENTION_SEC`(기본 86400초, 1일) 지난 파일부터 자동 삭제됩니다.
