# tmp_raspi_server 개발 노트

## 2026-02-25

- 라즈베리 파이 AP 모드 구성 및 ESP8266 ↔ STM32 ↔ Pi 통신 흐름 정리 (`AP_SETUP.md`).
- `bbox_to_esp.h/.cpp` 추가
  - `Camera/get_metadata/src/XMLParser.cpp` 의 `<tt:Object>` 파싱 로직을 참고해, XML에서 첫 번째 **Human** 객체의 정규화 좌표(0.0~1.0)를 추출하는 `extractFirstHumanCenter()` 구현.
  - 추출한 중심 좌표를 ESP8266 쪽 TCP 엔드포인트(예: `192.168.4.x:포트`)로 `"CX=...,CY=...\\n"` 형식의 텍스트로 전송하는 `sendCenterToEsp()` 구현.

