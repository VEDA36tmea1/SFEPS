# network

이 폴더는 Auth, Audio, Alert, Position, Video Catalog 같은 TCP 계열 서비스에서 공통으로 쓰는 네트워크 보조 기능을 제공합니다.

## 파일

### `net_utils.cpp`
- `is_ip_allowed`
  - allowlist 기반 접속 허용 여부를 판별합니다.
- `peer_ip_to_string`
  - `sockaddr_in`에서 사람이 읽을 수 있는 IPv4 문자열을 만듭니다.
- `apply_socket_read_timeout`
  - 소켓에 `SO_RCVTIMEO`를 적용합니다.
- `create_listen_socket`
  - bind IP와 포트를 받아 TCP listen 소켓을 생성합니다.
- `send_all_plain`
  - 일반 TCP 소켓으로 데이터를 끝까지 전송합니다.
- `send_all_tls`
  - `tls_write`를 반복 호출해 TLS 연결로 데이터를 끝까지 전송합니다.

### `tls_server.cpp`
- OpenSSL 기반 서버 소켓 초기화와 클라이언트 handshake 처리를 담당합니다.
- `init_tls_server`
  - TLS 1.2 이상을 강제하고 cert/key를 로드한 뒤 listen 소켓을 엽니다.
- `accept_tls_client`
  - non-blocking handshake + `poll()` 기반 타임아웃 제어로 TLS 클라이언트를 수락합니다.
- `tls_read`, `tls_write`
  - OpenSSL 오류를 POSIX 스타일 반환값/`errno`로 매핑합니다.
- `close_tls_client`, `close_tls_server`
  - TLS 자원과 파일 디스크립터를 정리합니다.

## 설계 포인트

- plain TCP와 TLS를 같은 서비스 코드에서 함께 다루기 쉽도록 송수신 인터페이스를 맞춰 둔 계층입니다.
- handshake 타임아웃은 `SecurityRuntimeOptions.app_tls_handshake_timeout_ms`에서 넘어오는 값을 사용합니다.
- bind 대상 IP를 명시적으로 받기 때문에 단일 NIC 또는 제한된 인터페이스 운영 환경에 맞추기 쉽습니다.
