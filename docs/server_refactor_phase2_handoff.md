# Server Refactor Phase 2 Handoff

Last updated: 2026-03-19

## Scope

- Target: `server/` only
- No source changes in `Camera/` and `client/`
- Public runtime contracts 유지:
  - TCP/TLS protocol strings unchanged
  - `.env.local` key names/semantics unchanged
  - `run_*` service entry contracts unchanged

## What Changed in Server

- Common utility layer 정리:
  - `text_utils` (trim/lower/sanitize)
  - `env_utils` (typed env parsing + allowlist parsing)
  - `sample_utils` (sampling helper)
- `main.cpp`는 orchestration 중심으로 단순화
  - runtime/security validation 분리
  - RFID image snapshot/finalize pipeline 분리
- 대형 모듈 내 중복 제거:
  - `analytics.cpp`: env/helper dedup + parsing helper 분리
  - `recorder.cpp`: runtime limits 로딩 정리
  - `rfid_monitor.cpp`: text helper 공통화
  - `log.cpp`: DB execute/bind/cleanup helper 분리
  - `alert.cpp`, `esp_manager.cpp`: dispatch/broadcast 중복 축소
- app services 분리:
  - `services/auth_service.cpp`
  - `services/audio_service.cpp`
  - `services/alert_service.cpp`
  - `services/position_service.cpp`
  - `services/video_catalog_service.cpp`

## Camera/Client Impact

- Camera:
  - 코드 변경 불필요
  - metadata contract 유지
- Client:
  - 즉시 코드 변경 필수는 아님
  - TLS-only 전환 단계에서 endpoint/reconnect 정책 점검 필요

## Verification

빌드 확인:

```bash
cmake -S server -B server/build
cmake --build server/build --clean-first -j4
```

결과:
- 성공 (`smart_server.bin` 링크 완료)
