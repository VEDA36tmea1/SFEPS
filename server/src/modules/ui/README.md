# ui

이 폴더는 클라이언트가 직접 접속해 조회하거나 구독하는 서비스들을 담고 있습니다. 현재 Position 스트림과 Video Catalog가 여기에 있습니다.

## 파일

### `position_service.cpp`
- 인증된 클라이언트만 Position 스트림에 접속할 수 있도록 합니다.
- 명령:
  - `SUB_POS|<object_id>`
  - `UNSUB_POS|<object_id>`
- 서버 푸시:
  - `OBJ_POS|...`
  - `OBJ_END|<object_id>|REASON=<...>`
- 주요 동작:
  - 인증되지 않은 IP는 연결 거부
  - 강제 로그아웃 Alert 이벤트 전송
  - 클라이언트별 활성 구독 object id 관리
  - `AnalyticsProcessor` snapshot을 주기적으로 읽어 변경분만 방송
  - Position 구독 대상과 `EspManager` 추적 대상을 동기화

### `video_catalog_service.cpp`
- 녹화 파일 목록 구독 및 `id` 기반 재생 서비스입니다.
- 연결 직후 스냅샷:
  - `REC_SNAPSHOT_BEGIN|TOTAL=<n>`
  - `REC|<id>|<created_at>`
  - `REC_SNAPSHOT_END|TOTAL=<n>`
- 연결 유지 중 실시간 갱신:
  - `REC_ADD|<id>|<created_at>`
  - `REC_DEL|<id>`
- 클라이언트 재생 요청:
  - `PLAY_REC|<id>`
- 서버 재생 응답:
  - `PLAY_URL|<id>|<created_at>|<url>`
  - 오류 시 `REC_ERR|<code>|<message>`, `PLAY_ERR|<code>|<message>`
- 주요 동작:
  - 연결 시 `recordings` 테이블을 조회하고 실제 파일이 존재하는 항목만 스냅샷으로 전송
  - 새 세그먼트 저장 완료 시 `REC_ADD` 이벤트를 푸시
  - cleanup 삭제 시 `REC_DEL` 이벤트를 푸시
  - `SFEPS_VIDEO_HTTP_BASE_URL`을 이용해 `PLAY_URL` 생성
  - 최대 동시 연결 수(`SFEPS_VIDEO_MAX_CLIENTS`)를 제한

## 운영 메모

- 두 서비스 모두 `core/network`와 `src/services`의 transport 공용 코드를 사용합니다.
- 현재 allowlist는 별도 키가 아니라 서비스 공용 정책을 재사용합니다.
- Video Catalog는 목록 전달만 앱 소켓으로 처리하고, 실제 MP4 전송은 외부 `/videos` HTTP 정적 서빙을 사용합니다.
