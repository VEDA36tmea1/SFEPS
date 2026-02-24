# SFEPS Server

SFEPS 서버는 영상 녹화, 로그인 인증, RFID 수신, 음성 재생, 알림 전송을 담당합니다.

## 런타임 필수 환경변수 (fail-closed)

서버 기동 전에 아래 값이 모두 설정되어 있어야 합니다.

```bash
export DB_SSL_CA=/etc/sfeps/pki/ca.crt
export RTSPS_TLS_CA=/etc/sfeps/pki/ca.crt

export SFEPS_DB_HOST=192.168.0.92
export SFEPS_DB_USER=pi
export SFEPS_DB_PASS='***'
export SFEPS_DB_NAME_AUTH=Client_db
export SFEPS_DB_NAME_ANALYTICS=CCgbd
```

누락된 값이 있으면 서버는 즉시 기동을 거부합니다.

## 보안 하드닝 선택형 환경변수

설정하지 않으면 아래 기본값으로 동작합니다.

```bash
export SFEPS_META_MAX_PACKET_BYTES=65536
export SFEPS_META_BAD_STREAK_LIMIT=20
export SFEPS_META_MAX_LINES_PER_BATCH=128
export SFEPS_ANALYTICS_QUEUE_MAX=200
export SFEPS_DROP_LOG_INTERVAL=100

export SFEPS_AUTH_MAX_BYTES=256
export SFEPS_AUDIO_MAX_BYTES=4194304
export SFEPS_ALERT_MAX_CLIENTS=64
export SFEPS_SOCKET_READ_TIMEOUT_MS=5000
```

포트 allowlist는 선택형입니다.
- 미설정: 호환성 모드(전체 허용, 시작 시 경고 로그 출력)
- 설정: exact IP만 허용 (콤마 구분)

```bash
export SFEPS_AUTH_ALLOW_IPS="192.168.0.10"
export SFEPS_AUDIO_ALLOW_IPS="192.168.0.11"
export SFEPS_ALERT_ALLOW_IPS="192.168.0.10,192.168.0.12"
```

## 핵심 포트

| 포트 | 용도 | 프로토콜 |
|---|---|---|
| 5555 | 로그인 인증 | TCP |
| 5556 | 음성 수신 | TCP |
| 5557 | 클라이언트 알림 | TCP |

## 빠른 실행

```bash
cd /home/iam/finalProject/SFEPS/server
mkdir -p build && cd build
cmake ..
make -j4

# 수동 실행(권장: run_server.sh 사용)
cd ..
./run_server.sh

# 테스트 알림 핑(2초 주기)
./run_server.sh --test-ping
```

## 주요 경로

- 영상 저장: `/home/iam/finalProject/SFEPS/videos`
- RFID 소켓: `/tmp/rc522_events.sock`

## 주요 기능

- RTSP 스트림을 60초 단위 MP4로 분할 저장
- RFID NDJSON 수신 및 이벤트 처리
- 로그인 인증 (`id:password`)
- 로그인 보호: 계정+IP 기준 5회 연속 실패 시 30초 서버 락아웃
- 로그인 요청 크기 제한 (`SFEPS_AUTH_MAX_BYTES`)
- 오디오 연결당 수신 바이트 상한 (`SFEPS_AUDIO_MAX_BYTES`)
- 알림 포트 동시 접속 수 상한 (`SFEPS_ALERT_MAX_CLIENTS`)
- 포트별 allowlist 기반 접속 제어 (`SFEPS_*_ALLOW_IPS`)
- 메타데이터 패킷/큐 상한 및 샘플링 드롭 로그 (`SFEPS_META_*`, `SFEPS_ANALYTICS_QUEUE_MAX`)
- 음성 RAW PCM 수신 후 `AudioRingBuffer + AudioPlayback(ALSA)` 경로로 재생
- 부정승차/테스트 메시지 알림 브로드캐스트
- Auth/Startup 정책: fail-closed (`Auth DB`, `logger`, `analytics` 실패 시 중단)

마지막 업데이트: 2026-02-24
