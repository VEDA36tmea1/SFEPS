# config

이 폴더는 서버 기동에 필요한 환경변수를 읽고, 안전한 기본값을 적용하고, 잘못된 설정이면 fail-closed로 막는 역할을 합니다.

## 파일

### `runtime_config.cpp`
- `RuntimeConfig`를 채우는 최소 DB 설정 로더입니다.
- 필수 환경변수:
  - `SFEPS_DB_USER`
  - `SFEPS_DB_PASS`
  - `SFEPS_DB_NAME_ANALYTICS`
- `SFEPS_DB_HOST`는 비어 있으면 `localhost`를 사용하고, 다른 값이면 기동을 거부합니다.
- 목적은 서버 DB 접근을 로컬 호스트로 제한하는 것입니다.

### `security_runtime.cpp`
- `SecurityRuntimeOptions`를 환경변수 기반으로 구성합니다.
- allowlist, 최대 바이트 수, 최대 클라이언트 수, 타임아웃, TLS 포트, ESP TCP 옵션, 이미지 보관 기간 등을 읽습니다.
- `validate_security_runtime_options`는 아래 항목을 검증합니다.
  - plaintext/TLS가 둘 다 비활성화되지 않았는지
  - bind IP가 올바른 IPv4인지
  - TLS cert/key 파일이 존재하고 읽기 가능한지
  - TLS 포트끼리, 또는 plaintext 포트와 충돌하지 않는지
  - Auth/Audio/Alert allowlist가 비어 있지 않은지
- `log_allowlist_mode`, `log_transport_mode`, `log_esp_transport_mode`는 현재 보안 모드를 기동 로그로 남깁니다.

## 운영 관점 메모

- 이 폴더 코드는 기본적으로 "잘못된 값이면 안전하게 실패"하는 방향을 따릅니다.
- Position/Video Catalog는 여기서 별도 allowlist를 갖지 않고 상위 서비스 정책과 함께 동작합니다.
- `env_utils.cpp`와 `text_utils.cpp`에 의존해 숫자/불리언/CSV/IP 문자열을 정리합니다.
