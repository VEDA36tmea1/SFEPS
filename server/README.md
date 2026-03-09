# SFEPS Server

SFEPS 서버는 영상 녹화, 로그인 인증, RFID 수신, 음성 재생, 알림 전송을 담당합니다.

## 런타임 필수 환경변수 (fail-closed)

서버 기동 전에 아래 값이 모두 설정되어 있어야 합니다.

```bash
export RTSPS_TLS_CA=/etc/sfeps/pki/ca.crt

export SFEPS_DB_HOST=localhost
export SFEPS_DB_USER=pi
export SFEPS_DB_PASS='***'
export SFEPS_DB_NAME_ANALYTICS=CCgbd
# 호환용(선택): 지정해도 런타임은 SFEPS_DB_NAME_ANALYTICS 단일 스키마를 사용
# export SFEPS_DB_NAME_AUTH=CCgbd
```

누락된 값이 있으면 서버는 즉시 기동을 거부합니다.
`SFEPS_DB_HOST`는 로컬 전용 모드로 `localhost`만 허용됩니다.

## VSCode DB 확장 접속(권장)

`localhost` 서버 정책은 유지하고, 개발 PC의 VSCode에서는 SSH 터널로 라즈베리파이 DB에 접속하세요.

### 1) 터널 실행

```bash
cd /home/iam/SFEPS/server
./tunnel_vscode_db.sh [pi-ip]
```

`[pi-ip]`가 생략되면 기본 `192.168.0.97`로 실행됩니다.

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
export SFEPS_META_MAX_LINES_PER_BATCH=128
export SFEPS_ANALYTICS_QUEUE_MAX=200
export SFEPS_DROP_LOG_INTERVAL=100

export SFEPS_AUTH_MAX_BYTES=256
export SFEPS_AUDIO_MAX_BYTES=4194304
export SFEPS_ALERT_MAX_CLIENTS=64
export SFEPS_SOCKET_READ_TIMEOUT_MS=5000
export SFEPS_RTSPS_VERIFYHOST=192.168.0.97

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

- `SFEPS_RTSPS_VERIFYHOST` 미설정 시 `RTSP_URL` 호스트를 자동 사용합니다.

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

# 테스트 알림 핑(2초 주기)
./run_server.sh --test-ping
```

## RFID 부팅 자동화 (systemd)

책임 분리:
- `hardware`: 부팅 시 `rc522` 모듈 로드 + RFID 데몬 서비스 제공
- `server`: RFID 소켓이 준비된 뒤에만 서버 시작

리소스 정책:
- `sfeps-rfid-module.service`만 부팅 자동시작(`enable`)합니다.
- `sfeps-rfid.service`는 **enable하지 않습니다**.
- `sfeps-server.service`가 시작될 때 의존성으로 `sfeps-rfid.service`가 같이 올라오고, 서버가 내려가면 함께 정지합니다.

### 1회 수동 적용

```bash
cd /home/iam/SFEPS

# 1) RFID 환경파일 배치(필요 시 값 수정)
sudo cp hardware/Raspi-driver/RC522_RFID/systemd/sfeps-rfid.env.example /etc/default/sfeps-rfid
sudo vi /etc/default/sfeps-rfid

# 2) hardware 서비스 유닛 설치
sudo cp hardware/Raspi-driver/RC522_RFID/systemd/sfeps-rfid-module.service /etc/systemd/system/sfeps-rfid-module.service
sudo cp hardware/Raspi-driver/RC522_RFID/systemd/sfeps-rfid.service /etc/systemd/system/sfeps-rfid.service

# 3) server 유닛 drop-in 설치 (소켓 준비 보장)
sudo mkdir -p /etc/systemd/system/sfeps-server.service.d
sudo cp server/systemd/sfeps-server.service.d/rfid.conf /etc/systemd/system/sfeps-server.service.d/rfid.conf

# 4) 반영
sudo systemctl daemon-reload

# 5) 모듈 로더만 부팅 자동시작
sudo systemctl enable --now sfeps-rfid-module.service

# 6) 서버 재시작 (서버가 sfeps-rfid.service를 on-demand로 기동)
sudo systemctl restart sfeps-server.service
```

### 확인 명령

```bash
systemctl is-active sfeps-rfid-module
systemctl is-active sfeps-server
systemctl is-active sfeps-rfid
lsmod | grep rc522
ls -l /dev/rc522
ls -l /tmp/rc522_events.sock
journalctl -u sfeps-rfid-module -u sfeps-rfid -u sfeps-server -b
```

### 검증 시나리오

1. 부팅 자동 동작
```bash
sudo reboot
# 재접속 후
systemctl is-active sfeps-rfid-module
lsmod | grep rc522
ls -l /dev/rc522
```

2. 서버 시작 순서/소켓 보장
```bash
sudo systemctl restart sfeps-server
systemctl is-active sfeps-rfid
ls -l /tmp/rc522_events.sock
journalctl -u sfeps-rfid -u sfeps-server -b | grep -E "Started|Starting|RFID|socket"
```

3. 실패 시 fail-closed
```bash
sudo sed -i 's#^SFEPS_RFID_KO_PATH=.*#SFEPS_RFID_KO_PATH=/bad/path/rc522.ko#' /etc/default/sfeps-rfid
sudo systemctl daemon-reload
sudo systemctl restart sfeps-rfid-module
systemctl is-failed sfeps-rfid-module
sudo systemctl restart sfeps-server
systemctl is-active sfeps-server

# 테스트 후 원복
sudo vi /etc/default/sfeps-rfid
sudo systemctl restart sfeps-rfid-module
sudo systemctl restart sfeps-server
```

4. 정상 태깅 경로
```bash
journalctl -u sfeps-server -f
# 카드 태깅 후 "RFID Tag" 로그 확인
```

## 주요 경로

- 영상 저장: `/home/iam/SFEPS/videos`
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
- XML 메타데이터 엄격 파싱(tinyxml2) + 필수 필드 검증(`RuleName`, `State`) + 필드 길이 제한
- 음성 RAW PCM 수신 후 `AudioRingBuffer + AudioPlayback(ALSA)` 경로로 재생
- 부정승차/테스트 메시지 알림 브로드캐스트
- DB 단일 스키마 모드: 인증/로그/분석 저장을 `SFEPS_DB_NAME_ANALYTICS`(예: `CCgbd`)로 통합
- Auth/Startup 정책: fail-closed (`Auth DB`, `logger`, `analytics` 실패 시 중단)

마지막 업데이트: 2026-02-24
