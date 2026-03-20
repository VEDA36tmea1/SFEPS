# alerts

이 폴더는 Alert 구독자 연결을 유지하고, 서버 전역 이벤트를 각 클라이언트로 브로드캐스트하는 기능을 담당합니다.

## 파일

### `alert.cpp`
- plain/TLS Alert 클라이언트 목록을 전역으로 관리합니다.
- `add_alert_plain_client`, `add_alert_tls_client`
  - 연결된 구독자를 등록합니다.
- `send_alert_to_clients`
  - 모든 구독자에게 메시지를 보냅니다.
- `send_alert_to_ip_clients`
  - 특정 IP에만 선택적으로 알림을 보냅니다.
- 전송 실패 시 해당 클라이언트를 목록에서 제거하고 연결을 닫습니다.

### `alert_service.cpp`
- Alert 서비스 리스너를 열고 새 구독 클라이언트를 받습니다.
- `start_listener_bundle`을 이용해 plaintext/TLS를 함께 지원합니다.
- `sec_cfg.alert_allow_ips` allowlist를 적용합니다.
- 최대 클라이언트 수(`SFEPS_ALERT_MAX_CLIENTS`)를 넘으면 연결을 거부합니다.

## 연결 관계

- `main.cpp`는 로그인 성공, fraud 판정, 강제 로그아웃, 이미지 URL 전송에 이 모듈을 사용합니다.
- `auth_service.cpp`, `analytics.cpp`, `position_service.cpp`가 모두 `send_alert_to_clients` 또는 `send_alert_to_ip_clients`를 호출합니다.

## 운영 메모

- 이 모듈은 구독형 push 채널입니다. 요청-응답 프로토콜보다 "연결 유지 + 서버 푸시"에 가깝습니다.
- TLS 연결도 내부적으로 같은 브로드캐스트 흐름에 합쳐서 관리합니다.
