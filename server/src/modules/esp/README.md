# esp

이 폴더는 ESP 장치와의 TCP 연동을 담당합니다. 주된 목적은 현재 추적 대상의 시작, 위치 변경, 종료 이벤트를 외부 장치로 전달하는 것입니다.

## 파일

### `esp_manager.cpp`
- `EspManager` 구현체입니다.
- `start`
  - 설정된 bind IP/port로 TCP 서버를 시작합니다.
- `acceptLoop`
  - 클라이언트 접속을 받고 allowlist/max client 정책을 적용합니다.
- `sendStartupReadyAfterDelay`
  - 서버 시작 후 5초 뒤 `ESP_READY|SERVER_ONLINE` 메시지를 보냅니다.
- `publishTrackStart`, `publishTrackPos`, `publishTrackEnd`
  - 추적 시작/위치/종료 이벤트를 브로드캐스트합니다.
- `publishTrackChangeSignal`
  - 추적 대상 전환 신호를 전송합니다.
- `publishFraudTrackPosIfIdle`
  - 사람이 직접 추적 중이 아닐 때 fraud 대상 위치를 자동 전파합니다.
- `expireFraudTrackIfStale`
  - 오래 갱신되지 않은 fraud 추적을 종료시킵니다.

## 연결 관계

- `main.cpp`가 `SecurityRuntimeOptions`로부터 ESP 설정을 읽어 `EspManager`를 생성합니다.
- `streaming/analytics.cpp`의 fraud 위치 콜백, `ui/position_service.cpp`의 수동 구독 흐름이 모두 이 모듈을 호출합니다.

## 운영 메모

- `SFEPS_ESP_TCP_ENABLE=0`이면 사실상 비활성 상태로 남습니다.
- allowlist가 비어 있으면 bound interface 범위 내 allow-all 방식으로 동작합니다.
