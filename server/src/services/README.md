# services

`server/src/services`는 개별 기능 모듈(`modules/`)이 공통으로 사용하는 서비스 인프라 계층입니다. 실제 Auth, Alert, Audio, Position, Video Catalog 구현은 `modules/`에 있지만, 그 구현들이 공유하는 포트/전송/프로토콜 헬퍼는 여기 모여 있습니다.

## 파일

### `app_services_impl.h`
- `bootstrap/app_services.cpp`가 호출하는 실제 구현 함수 선언부입니다.
- 아래 서비스 구현 진입점을 공통 네임스페이스로 노출합니다.
  - `run_audio_receiver_impl`
  - `run_fraud_notifier_impl`
  - `run_position_stream_service_impl`
  - `run_login_auth_impl`
  - `run_video_catalog_service_impl`
- 역할 자체는 작지만, `core/bootstrap`과 `modules/*_service.cpp` 사이 계약을 고정하는 헤더입니다.

### `service_shared.h` / `service_shared.cpp`
- 여러 서비스가 함께 쓰는 상수와 문자열/프로토콜 유틸을 제공합니다.
- 포함 내용:
  - 기본 포트 상수 (`5555`~`5558`)
  - Position/Object ID 관련 포맷 함수
  - 인증된 IP 세션 집합 관리
  - Video Catalog 요청 파서
  - MySQL literal escape
  - 에러/시간/URL 문자열 정규화
- 대표 함수:
  - `mark_ip_authenticated`, `unmark_ip_authenticated`, `is_ip_authenticated`
  - `format_obj_pos_line`, `format_obj_end_line`
  - `parse_video_catalog_request`
  - `join_http_url`

### `transport_utils.h` / `transport_utils.cpp`
- plain TCP와 TLS를 서비스 코드에서 같은 방식으로 다루기 위한 공용 전송 계층입니다.
- 포함 내용:
  - `ListenerBundle`
    - plain 리스너 FD와 TLS 서버를 한 구조체로 묶습니다.
  - `AcceptedClient`
    - plain/TLS 클라이언트 정보를 공통 구조로 표현합니다.
  - `start_listener_bundle`
    - 서비스별 plain/TLS 리스너를 한 번에 시작합니다.
  - `accept_client`
    - 연결 수락, peer IP 확인, allowlist 적용까지 처리합니다.
  - `client_read`, `client_send_all`, `client_send_line`
    - plain/TLS 차이를 숨기고 동일 인터페이스로 송수신합니다.
  - `close_client`
    - 연결 종류에 맞춰 안전하게 자원을 정리합니다.

## 다른 계층과의 관계

### `core/bootstrap`
- `core/bootstrap/app_services.cpp`는 여기 선언된 `*_impl` 함수로만 실제 서비스 시작을 위임합니다.

### `modules`
- `modules/auth`, `modules/alerts`, `modules/ui`, `modules/voice` 구현들이 `service_shared.*`, `transport_utils.*`를 직접 사용합니다.

### `core/network`
- `transport_utils.cpp`는 내부적으로 `create_listen_socket`, `send_all_plain`, `send_all_tls`, `accept_tls_client` 같은 `core/network` 기능을 조합해서 씁니다.

## 읽는 순서 추천

1. `app_services_impl.h`
   - 서비스 구현 진입점 이름을 먼저 파악
2. `transport_utils.h`
   - 서비스 공통 네트워크 추상화 이해
3. `service_shared.h`
   - 인증 상태, Position 포맷, Video Catalog 파싱 규칙 확인

## 설계 포인트

- 서비스 구현 코드가 plain/TLS 분기와 공통 파싱 로직을 매번 직접 쓰지 않도록 중간 계층을 둔 구조입니다.
- 새 TCP/TLS 서비스가 늘어나면 보통 이 디렉토리 유틸을 재사용해 `modules/`에 구현을 추가하는 흐름이 됩니다.
