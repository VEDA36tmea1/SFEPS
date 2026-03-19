# Server app_services 리팩터링 전달사항

최종 갱신: 2026-03-19

## 배경

- `server/src/app_services.cpp`를 얇은 엔트리로 축소
- 실제 로직은 `server/src/services/*`로 분리
- Plain/TLS 공통 transport 계층(`transport_utils.*`)으로 통합
- 외부 프로토콜 문자열(`PASS/FAIL`, `OBJ_POS|...`, `REC|...`)은 유지

## 현재 서비스 구성

- `server/src/services/auth_service.cpp`
- `server/src/services/audio_service.cpp`
- `server/src/services/alert_service.cpp`
- `server/src/services/position_service.cpp`
- `server/src/services/video_catalog_service.cpp`
- 공통:
  - `service_shared.*`
  - `transport_utils.*`

## 보안/운영 반영 사항

- allowlist 정책은 현재 fail-closed
  - 필수: `SFEPS_AUTH_ALLOW_IPS`, `SFEPS_AUDIO_ALLOW_IPS`, `SFEPS_ALERT_ALLOW_IPS`
- Position/VideoCatalog는 `SFEPS_ALERT_ALLOW_IPS`를 공유
- TLS/Plain 동시 운영 시 포트 충돌 검증 추가

## client 담당자 전달사항 (코드 수정 없음)

- 지금은 plain/TLS 동시 지원 가능
- 향후 plain 제거 전 확인 항목:
  - auth/audio/alert/position/video 모두 TLS 경로 접속 검증
  - TLS handshake 실패/타임아웃 처리 정책 검증
  - allowlist 거절/서버 재시작 시 재시도 백오프 검증

## Camera 담당자 전달사항 (코드 수정 없음)

- 이번 변경은 서버 내부 분리 중심
- Camera 연동 계약(입출력/프로토콜)은 변경 없음
