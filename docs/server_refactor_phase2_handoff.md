# Server Refactor Phase 2 Handoff

## Scope
- Target: `server/` only.
- No source changes in `Camera/` and `client/`.
- Public runtime contract was kept:
  - TCP/TLS protocol strings unchanged
  - `.env.local` key names/semantics unchanged
  - `run_*` service entry contracts unchanged

## What Changed in Server
- Common utility layer added and reused:
  - `text_utils` (trim/lower/sanitize)
  - `env_utils` (typed env parsing + allowlist parsing)
  - `sample_utils` (sampling helper)
- `main.cpp` reduced to orchestration-only flow.
  - security/runtime loading+validation moved to `security_runtime`.
  - RFID image snapshot/finalize pipeline moved to `rfid_image_pipeline`.
- Large-module internal slimming:
  - `analytics.cpp`: env/helper dedup + event parsing helper extraction.
  - `recorder.cpp`: env/helper dedup + runtime limit loading grouped.
  - `rfid_monitor.cpp`: text helper dedup.
  - `log.cpp`: DB execute/bind/cleanup helper separation.
  - `alert.cpp`, `esp_manager.cpp`: client dispatch/broadcast duplication reduced.
- Legacy cleanup:
  - removed `server/include/event_matcher.h`
  - removed `server/src/event_matcher.cpp`

## Camera/Client Impact
- Camera:
  - No code change required from this refactor.
  - Existing metadata contract with server remains the same.
- Client:
  - No immediate code change required from this refactor.
  - For future plaintext removal phase, validate TLS-only endpoint/reconnect policy.

## Verification
- Build verified:
  - `cmake -S server -B server/build`
  - `cmake --build server/build --clean-first -j4`
- Result: success (`smart_server.bin` linked).
