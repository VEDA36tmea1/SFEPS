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
```

## 주요 경로

- 영상 저장: `/home/iam/finalProject/SFEPS/videos`
- RFID 소켓: `/tmp/rc522_events.sock`

## 주요 기능

- RTSP 스트림을 60초 단위 MP4로 분할 저장
- RFID NDJSON 수신 및 이벤트 처리
- 로그인 인증 (`id:password`)
- 로그인 보호: 계정+IP 기준 5회 연속 실패 시 30초 서버 락아웃
- 음성 RAW PCM 수신 후 `aplay`로 즉시 재생
- 부정승차/테스트 메시지 알림 브로드캐스트

마지막 업데이트: 2026-02-23
