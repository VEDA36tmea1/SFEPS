# modules

`server/src/modules`는 실제 서비스 기능이 구현되는 계층입니다. `core`가 설정, 네트워크 공통 기반, 운영성 작업을 제공하면, `modules`는 인증, 알림, 영상 분석, UI 스트림, 음성 같은 도메인 기능을 담당합니다.

## 디렉토리 구성

### `alerts/`
- Alert 구독 클라이언트 관리와 브로드캐스트 전송을 담당합니다.

### `auth/`
- 사용자 인증 DB 조회와 Auth 서비스 포트 처리를 담당합니다.

### `streaming/`
- RTSP 녹화, 메타데이터 분석, RFID 이벤트 연결, fraud 이미지 파이프라인을 담당합니다.

### `ui/`
- Position 스트림과 Video Catalog 같은 클라이언트 조회성 서비스를 담당합니다.

### `voice/`
- 오디오 수신 및 재생 버퍼링을 담당합니다.

## `main.cpp` 기준 실행 흐름

1. `streaming/analytics.cpp`의 `AnalyticsProcessor`를 시작합니다.
2. `voice/`, `alerts/`, `auth/`, `ui/` 서비스가 각각 TCP/TLS 리스너로 동작합니다.
3. `streaming/recorder.cpp`가 RTSP 비디오와 메타데이터를 읽고 분석 파이프라인으로 넘깁니다.
4. `streaming/rfid_monitor.cpp`와 `rfid_image_pipeline.cpp`가 RFID 이벤트와 이미지 스냅샷 흐름을 연결합니다.

## 읽는 순서 추천

- 전체 fraud 판정 흐름을 보려면 `streaming/README.md`
- 클라이언트 프로토콜을 보려면 `auth/README.md`, `ui/README.md`, `alerts/README.md`
