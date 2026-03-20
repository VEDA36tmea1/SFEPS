# bootstrap

이 폴더는 서비스 실행 진입점을 얇게 감싸는 부트스트랩 계층입니다. `main.cpp`는 여기 선언된 함수를 호출하고, 실제 구현은 `server/src/services/app_services_impl.*`로 위임됩니다.

## 파일

### `app_services.cpp`
- `run_audio_receiver`
  - 오디오 수신 서비스 시작을 구현 계층으로 전달합니다.
- `run_fraud_notifier`
  - Alert 브로드캐스트 서비스를 시작합니다.
- `run_video_catalog_service`
  - 런타임 설정과 보안 설정을 받아 녹화 목록 조회 서비스를 시작합니다.
- `run_position_stream_service`
  - `AnalyticsProcessor`, `EspManager`를 연결한 위치 스트리밍 서비스를 시작합니다.
- `run_login_auth`
  - 로그인 인증 서비스를 시작합니다.

## 역할 정리

- `main.cpp`가 서비스 구현 세부사항을 직접 알지 않도록 경계를 만들어 줍니다.
- 공용 헤더(`app_services.h`)에 정의된 `SecurityRuntimeOptions`를 서비스 시작 인자에 맞춰 전달합니다.
- 서비스 구현 파일 경로가 바뀌더라도 진입점 계약을 안정적으로 유지할 수 있습니다.

## 수정할 때 주의할 점

- 이 레이어는 "로직 구현"보다 "호출 연결"에 집중해야 합니다.
- 실제 네트워크 처리, 인증, 스트리밍 로직은 가능하면 `src/services` 쪽에 두는 편이 구조상 깔끔합니다.
