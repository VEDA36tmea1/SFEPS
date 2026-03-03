# tmp_raspi_server 개발 노트

## 2026-02-25

- 라즈베리 파이 AP 모드 구성 및 ESP8266 ↔ STM32 ↔ Pi 통신 흐름 정리 (`AP_SETUP.md`).
- `bbox_to_esp.h/.cpp` 추가
  - `Camera/get_metadata/src/XMLParser.cpp` 의 `<tt:Object>` 파싱 로직을 참고해, XML에서 첫 번째 **Human** 객체의 정규화 좌표(0.0~1.0)를 추출하는 `extractFirstHumanCenter()` 구현.
  - 추출한 중심 좌표를 ESP8266 쪽 TCP 엔드포인트(예: `192.168.4.x:포트`)로 `"CX=...,CY=...\\n"` 형식의 텍스트로 전송하는 `sendCenterToEsp()` 구현.

## 2026-02-26

- `camera_meta_test` 추가
  - `camera_config.h`, `rtsp_client_simple.*` 를 사용해 **DB 없이** 카메라 메타데이터만 받아서 Human 바운딩 박스 중심 좌표를 콘솔에 출력하는 테스트용 바이너리 구현.
- `raspi_tcp_server` 확장
  - 기본 모드: 키보드로 입력한 좌표/문자열을 ESP8266(TCP 클라이언트)로 전송하는 테스트 서버.
  - `./raspi_tcp_server rtt [port]` 실행 시: PING/PONG 라인 echo 기반 **RTT 측정 모드** 추가 (평균/최소/최대 RTT 및 편도 지연 추정 로그).

