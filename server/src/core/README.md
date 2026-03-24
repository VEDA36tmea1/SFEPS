# core

`server/src/core`는 서버의 공통 런타임 기반 계층입니다. `main.cpp`가 직접 사용하는 설정 로딩, 네트워크 유틸리티, 백그라운드 정리 작업, 서비스 부트스트랩이 여기 모여 있습니다.

## 디렉토리 구성

### `bootstrap/`
- 앱 서비스 시작 함수를 한곳에 모아 `main.cpp`와 실제 구현(`src/services`) 사이를 연결합니다.

### `config/`
- DB 접속 정보와 보안/네트워크 런타임 옵션을 환경변수에서 읽고 검증합니다.

### `network/`
- TCP/TLS 서버 공통 처리, 송수신 보조 함수, 소켓 생성 유틸리티를 제공합니다.

### `ops/`
- 로그 적재와 파일/이미지 정리처럼 장시간 실행되는 운영성 작업을 담당합니다.

### `utils/`
- 여러 모듈이 재사용하는 문자열/환경변수 파싱 함수들을 제공합니다.

## `main.cpp`에서의 사용 흐름

1. `config/`에서 `RuntimeConfig`, `SecurityRuntimeOptions`를 로드하고 검증합니다.
2. `ops/log.cpp`의 `DBLogger`를 시작해 로그인/녹화 로그 기록을 비동기로 처리합니다.
3. `ops/cleanup.cpp`의 워커 스레드를 띄워 오래된 영상/이미지를 정리합니다.
4. `bootstrap/app_services.cpp`를 통해 Auth, Audio, Alert, Position, Video Catalog 서비스를 시작합니다.
5. 각 서비스 구현은 필요 시 `network/`, `utils/` 계층을 재사용합니다.

## 문서 읽는 순서

- 설정부터 이해하려면 `config/README.md`
- 실제 서버 포트 처리 방식을 보려면 `network/README.md`
- 백그라운드 운영 작업을 보려면 `ops/README.md`
