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
- 녹화 파일 목록 조회 서비스입니다.
- 요청 형식:
  - `LIST_REC|FROM=<...>|TO=<...>|Q=<...>|PAGE=<n>|SIZE=<n>`
- 응답 형식:
  - `REC|<id>|<created_at>|0|<play_url>`
  - `REC_END|PAGE=<n>|SIZE=<n>|TOTAL=<n>|HAS_NEXT=<0|1>`
  - 오류 시 `REC_ERR|<code>|<message>`
- 주요 동작:
  - `recordings` 테이블을 조회하고 실제 파일이 존재하는 항목만 반환
  - `SFEPS_VIDEO_HTTP_BASE_URL`을 이용해 재생 URL 생성
  - 페이지네이션과 텍스트 검색을 처리
  - 최대 동시 요청 수(`SFEPS_VIDEO_MAX_CLIENTS`)를 제한

## 운영 메모

- 두 서비스 모두 `core/network`와 `src/services`의 transport 공용 코드를 사용합니다.
- 현재 allowlist는 별도 키가 아니라 서비스 공용 정책을 재사용합니다.
