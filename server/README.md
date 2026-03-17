# SFEPS Server

SFEPS 서버는 영상 녹화, 로그인 인증, RFID 수신, 음성 재생, 알림 전송을 담당합니다.

## 런타임 필수 환경변수 (fail-closed)

서버 기동 전에 아래 값이 모두 설정되어 있어야 합니다.

```bash
export SFEPS_DB_HOST=localhost
export SFEPS_DB_USER=pi
export SFEPS_DB_PASS='***'
export SFEPS_DB_NAME_ANALYTICS=CCgbd
# 호환용(선택): 지정해도 런타임은 SFEPS_DB_NAME_ANALYTICS 단일 스키마를 사용
# export SFEPS_DB_NAME_AUTH=CCgbd
```

누락된 값이 있으면 서버는 즉시 기동을 거부합니다.
`SFEPS_DB_HOST`는 로컬 전용 모드로 `localhost`만 허용됩니다.
서버는 카메라 스트림을 로컬 MediaMTX의 `rtsp://127.0.0.1:8554/cam1`에서 읽습니다.

## VSCode DB 확장 접속(권장)

`localhost` 서버 정책은 유지하고, 개발 PC의 VSCode에서는 SSH 터널로 라즈베리파이 DB에 접속하세요.

### 1) 터널 실행

```bash
cd /home/iam/SFEPS/server
./tunnel_vscode_db.sh [pi-ip]
```

`[pi-ip]`가 생략되면 기본 `192.168.0.101`로 실행됩니다.

### 2) VSCode 연결 정보

```text
Host: 127.0.0.1
Port: 33060
User: pi
Password: (MariaDB 비밀번호)
Database: CCgbd
```

터널이 살아 있는 상태에서 VSCode DB 확장에 추가 연결하면 라즈베리파이 DB를 조회할 수 있습니다.

### 참고
- `127.0.0.1:33060` 접속은 로컬 포트 포워딩으로 동작하므로 DB 서버의 외부 노출은 유지할 필요가 없습니다.
- DB 서버에서 LAN 직접 접속용 바인드/권한 변경 없이도 동작합니다.

## 보안 하드닝 선택형 환경변수

설정하지 않으면 아래 기본값으로 동작합니다.

```bash
export SFEPS_META_MAX_PACKET_BYTES=65536
export SFEPS_META_BAD_STREAK_LIMIT=20
export SFEPS_META_XML_BUFFER_MAX=1048576
export SFEPS_META_XML_DOC_MAX_BYTES=262144
export SFEPS_META_MAX_LINES_PER_BATCH=128
export SFEPS_ANALYTICS_QUEUE_MAX=200
export SFEPS_DROP_LOG_INTERVAL=100

export SFEPS_AUTH_MAX_BYTES=256
export SFEPS_AUDIO_MAX_BYTES=4194304
export SFEPS_ALERT_MAX_CLIENTS=64
export SFEPS_SOCKET_READ_TIMEOUT_MS=5000
export SFEPS_AUTH_DEAUTH_GRACE_MS=3000

export SFEPS_APP_TLS_ENABLE=0
export SFEPS_APP_PLAINTEXT_ENABLE=1
export SFEPS_AUTH_TLS_PORT=6555
export SFEPS_AUDIO_TLS_PORT=6556
export SFEPS_ALERT_TLS_PORT=6557
export SFEPS_APP_TLS_HANDSHAKE_TIMEOUT_MS=3000
# TLS 활성 시 필수
# export SFEPS_APP_TLS_CERT_FILE=/etc/sfeps/pki/server.crt
# export SFEPS_APP_TLS_KEY_FILE=/etc/sfeps/pki/server.key
```

- `smart_server -> MediaMTX`는 로컬 루프백 `127.0.0.1:8554` 평문 RTSP를 사용합니다.
- Qt 등 외부 앱이 서버에 붙는 구간은 `SFEPS_APP_TLS_ENABLE=1`일 때 TLS로 보호됩니다.
- `SFEPS_AUTH_DEAUTH_GRACE_MS`는 Position 연결 종료 후 인증 IP 해제까지의 유예 시간(ms)입니다.
- 메타데이터 XML은 packet 단위가 아닌 document 단위로 재조립 후 파싱합니다.
- `SFEPS_META_XML_DOC_MAX_BYTES`는 `SFEPS_META_XML_BUFFER_MAX` 이하로 설정하세요.

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

TLS 점진 전환(dual-stack) 시 기본 TLS 포트:

| 포트 | 용도 | 프로토콜 |
|---|---|---|
| 6555 | 로그인 인증(TLS) | TLS/TCP |
| 6556 | 음성 수신(TLS) | TLS/TCP |
| 6557 | 클라이언트 알림(TLS) | TLS/TCP |

## 앱 포트 TLS 점진 전환

서버는 3가지 모드를 지원합니다.

1. plain only
```bash
export SFEPS_APP_TLS_ENABLE=0
export SFEPS_APP_PLAINTEXT_ENABLE=1
```

2. dual-stack (권장 1차 롤아웃)
```bash
export SFEPS_APP_TLS_ENABLE=1
export SFEPS_APP_PLAINTEXT_ENABLE=1
export SFEPS_AUTH_TLS_PORT=6555
export SFEPS_AUDIO_TLS_PORT=6556
export SFEPS_ALERT_TLS_PORT=6557
export SFEPS_APP_TLS_CERT_FILE=/etc/sfeps/pki/server.crt
export SFEPS_APP_TLS_KEY_FILE=/etc/sfeps/pki/server.key
```

3. TLS-only (2차 전환)
```bash
export SFEPS_APP_TLS_ENABLE=1
export SFEPS_APP_PLAINTEXT_ENABLE=0
```

주의:
- `SFEPS_APP_TLS_ENABLE=1`이면 cert/key 경로가 필수이며 읽기 가능해야 합니다.
- plain 활성 상태(`SFEPS_APP_PLAINTEXT_ENABLE=1`)에서는 TLS 포트가 `5555/5556/5557`과 겹치면 기동 실패합니다.
- TLS 인증서는 SAN에 실제 접속 IP/DNS를 포함해야 합니다.

## 빠른 실행

```bash
cd /home/iam/SFEPS/server
mkdir -p build && cd build
cmake ..
make -j4

# 수동 실행(권장: run_server.sh 사용)
cd ..
./run_server.sh
```

## analytics_logs 1회 DDL 적용 (ObjectId + RFID Fraud 전용)

주의:
- 이 작업은 기존 `analytics_logs` 데이터를 즉시 삭제합니다.
- 1회성 작업이므로 SQL 파일 없이 터미널에서 직접 실행해도 됩니다.

```bash
cd /home/iam/SFEPS/server

# .env.local 사용 시
set -a
source .env.local
set +a

mysql -u"$SFEPS_DB_USER" -p"$SFEPS_DB_PASS" "$SFEPS_DB_NAME_ANALYTICS" \
  -e "DROP TABLE IF EXISTS analytics_logs; \
      CREATE TABLE analytics_logs ( \
        id INT(11) NOT NULL AUTO_INCREMENT, \
        object_id VARCHAR(128) NOT NULL, \
        card_age_text VARCHAR(64) NOT NULL DEFAULT '0', \
        age VARCHAR(32) NOT NULL DEFAULT '20s', \
        is_fraud TINYINT(1) NOT NULL DEFAULT 0, \
        created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, \
        PRIMARY KEY (id), \
        KEY idx_analytics_logs_object_id (object_id), \
        KEY idx_analytics_logs_created_at (created_at), \
        KEY idx_analytics_logs_is_fraud (is_fraud) \
      ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;"
```

검증:

```bash
mysql -u"$SFEPS_DB_USER" -p"$SFEPS_DB_PASS" "$SFEPS_DB_NAME_ANALYTICS" \
  -e "SHOW CREATE TABLE analytics_logs\G"
```

## RFID 부팅 자동화 (systemd)

목표 동작:
- 부팅 시 `sfeps-rfid-module.service`가 `rc522` 모듈을 자동 로드
- 서버 시작 시 `sfeps-rfid.service`가 소켓(`/tmp/rc522_events.sock`) 생성
- 서버 종료 시 `sfeps-rfid.service`가 정지되고 소켓 자동 삭제

### 1회 수동 적용

```bash
cd /home/iam/SFEPS

# 1) RFID 환경파일 배치(필요 시 값 수정)
sudo cp hardware/Raspi-driver/RC522_RFID/systemd/sfeps-rfid.env.example /etc/default/sfeps-rfid
sudo vi /etc/default/sfeps-rfid

# 2) hardware 서비스 유닛 설치
sudo cp hardware/Raspi-driver/RC522_RFID/systemd/sfeps-rfid-module.service /etc/systemd/system/sfeps-rfid-module.service
sudo cp hardware/Raspi-driver/RC522_RFID/systemd/sfeps-rfid.service /etc/systemd/system/sfeps-rfid.service

# 3) image processing 서비스 설치
sudo cp server/systemd/sfeps-image-processing.service /etc/systemd/system/sfeps-image-processing.service

# 4) server 본 유닛 + drop-in 설치
sudo cp server/systemd/sfeps-server.service /etc/systemd/system/sfeps-server.service
sudo mkdir -p /etc/systemd/system/sfeps-server.service.d
sudo cp server/systemd/sfeps-server.service.d/rfid.conf /etc/systemd/system/sfeps-server.service.d/rfid.conf
sudo cp server/systemd/sfeps-server.service.d/image-processing.conf /etc/systemd/system/sfeps-server.service.d/image-processing.conf

# 5) 반영
sudo systemctl daemon-reload

# 6) 자동시작
sudo systemctl enable --now sfeps-rfid-module.service
sudo systemctl enable --now sfeps-server.service
```

### 확인 명령

```bash
systemctl is-active sfeps-rfid-module
systemctl is-active sfeps-server
systemctl is-active sfeps-rfid
systemctl is-active sfeps-image-processing
lsmod | grep rc522
ls -l /dev/rc522
ls -l /tmp/rc522_events.sock
journalctl -u sfeps-rfid-module -u sfeps-rfid -u sfeps-image-processing -u sfeps-server -b
```

### 검증 시나리오

1. 부팅 자동 동작
```bash
sudo reboot
# 재접속 후
systemctl is-active sfeps-rfid-module
systemctl is-active sfeps-server
lsmod | grep rc522
ls -l /dev/rc522
```

2. 서버 시작 시 소켓 생성
```bash
sudo systemctl start sfeps-server
systemctl is-active sfeps-rfid
ls -l /tmp/rc522_events.sock
```

3. 서버 종료 시 소켓 소멸
```bash
sudo systemctl stop sfeps-server
systemctl is-active sfeps-rfid
ls -l /tmp/rc522_events.sock
```

4. 실패 시 fail-closed
```bash
sudo sed -i 's#^SFEPS_RFID_KO_PATH=.*#SFEPS_RFID_KO_PATH=/bad/path/rc522.ko#' /etc/default/sfeps-rfid
sudo systemctl daemon-reload
sudo systemctl restart sfeps-rfid-module
systemctl is-failed sfeps-rfid-module
sudo systemctl start sfeps-server
systemctl is-active sfeps-server

# 테스트 후 원복
sudo vi /etc/default/sfeps-rfid
sudo systemctl restart sfeps-rfid-module
sudo systemctl restart sfeps-server
```

5. 정상 태깅 경로
```bash
journalctl -u sfeps-server -f
# 카드 태깅 후 "RFID Tag" 로그 확인
```

## 주요 경로

- 영상 저장: `/home/iam/SFEPS/videos`
- 부정승차 이벤트 이미지 pending: `/home/iam/SFEPS/event_images/pending`
- 부정승차 이벤트 이미지 keep(fraud): `/home/iam/SFEPS/event_images/fraud`
- 부정승차 이벤트 이미지 failed(예외): `/home/iam/SFEPS/event_images/failed`
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
- 메타데이터 패킷/큐 상한 및 XML 재조립 제한 + 샘플링 드롭 로그 (`SFEPS_META_*`, `SFEPS_ANALYTICS_QUEUE_MAX`)
- XML 메타데이터에서 `Type=Human` 객체 `ObjectId`를 pending으로 등록하고 RFID와 FIFO 매칭
- RFID `text`가 `성인/adult`가 아니면 부정승차(`fraud=Y`)로 판정
- RFID 매칭 시점에 `3_best_shot.jpg`를 `object_id` 기반 파일명으로 pending 스냅샷 보관
- outline 판정 시 `fraud=N`은 pending 즉시 삭제, `fraud=Y`는 fraud 디렉터리로 이동 보존
- 이미지 운영 로그 키워드: `RFID_IMAGE_SNAP`, `RFID_IMAGE_DELETE`, `RFID_IMAGE_KEEP`
- `fraud=Y` 건만 `analytics_logs(object_id, card_age_text, age, is_fraud, created_at)`에 저장
- 알림 포맷: `FRAUD|object_id|card_age_text|age_group|is_fraud`
- 음성 RAW PCM 수신 후 `AudioRingBuffer + AudioPlayback(ALSA)` 경로로 재생
- 부정승차/테스트 메시지 알림 브로드캐스트
- DB 단일 스키마 모드: 인증/로그/분석 저장을 `SFEPS_DB_NAME_ANALYTICS`(예: `CCgbd`)로 통합
- Auth/Startup 정책: fail-closed (`Auth DB`, `logger`, `analytics` 실패 시 중단)

마지막 업데이트: 2026-03-09
