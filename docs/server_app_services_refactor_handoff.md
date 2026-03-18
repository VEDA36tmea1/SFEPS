# Server app_services 리팩터링 전달사항

## 배경
- 서버 `app_services.cpp`를 기능별 파일로 분해하고, Plain/TLS 공통 transport 계층으로 정리했습니다.
- 외부 프로토콜/응답 포맷/환경변수 키는 유지했습니다.

## client 담당자 전달사항 (코드 수정 없음)
- 현재는 `SFEPS_APP_PLAINTEXT_ENABLE`과 TLS를 동시에 지원하지만, 서버 구조는 TLS 우선으로 정리되었습니다.
- 향후 Plain 제거 전 점검 필요 항목:
  - auth/audio/alert/position/video 접속 모두 TLS 포트 경로 사용 확인
  - 재연결 시 TLS handshake 실패/타임아웃 처리 정책 확인
  - allowlist 거절/서버 재시작 시 재시도 백오프 동작 확인
- 이번 리팩터링에서 프로토콜 문자열(`PASS/FAIL`, `OBJ_POS|...`, `REC|...`) 변경은 없습니다.

## Camera 담당자 전달사항 (코드 수정 없음)
- Camera 연동 계약은 이번 변경으로 영향을 받지 않습니다.
- 서버 내부 구조만 분해되었고, Camera 경로/프로토콜/입출력 인터페이스 변경은 없습니다.

## 참고
- 서버 기능 분리 결과:
  - `server/src/services/audio_service.cpp`
  - `server/src/services/alert_service.cpp`
  - `server/src/services/video_catalog_service.cpp`
  - `server/src/services/position_service.cpp`
  - `server/src/services/auth_service.cpp`
  - 공통: `service_shared.*`, `transport_utils.*`
