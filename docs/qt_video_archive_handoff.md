# Qt 전달용: 서버 녹화영상 찾아보기(Video Catalog) 연동 안내

최종 갱신: 2026-03-24

## 1) 목적

- Qt 클라이언트가 Video Catalog 소켓에 연결되면 서버가 저장된 녹화 목록을 즉시 push
- 연결 유지 중 새로 저장되는 녹화/삭제되는 녹화도 실시간 반영
- 목록 화면에는 `id`, `created_at`만 표시
- 사용자가 항목을 선택하면 Qt는 `id`만 서버에 보내고, 서버는 `PLAY_URL`로 실제 HTTP 재생 URL 반환

## 2) 서버 요청/응답 프로토콜

### 2-1. 연결 직후 서버 스냅샷 push

- 클라이언트가 Video Catalog 포트에 연결되면 서버가 먼저 전체 목록을 보냅니다.
- 한 줄 텍스트 프로토콜이며 개행 `\n`으로 구분합니다.

형식:

```text
REC_SNAPSHOT_BEGIN|TOTAL=<n>
REC|<id>|<created_at>
REC|<id>|<created_at>
...
REC_SNAPSHOT_END|TOTAL=<n>
REC_STORAGE|USED_BYTES=<n>|TOTAL_BYTES=<n>|AVAILABLE_BYTES=<n>|FILE_COUNT=<n>
```

필드:
- `id`: `recordings.id`
- `created_at`: ISO8601 (`YYYY-MM-DDTHH:MM:SS`)

예시:

```text
REC_SNAPSHOT_BEGIN|TOTAL=3
REC|3624|2026-03-24T16:27:24
REC|3623|2026-03-24T16:26:24
REC|3622|2026-03-24T16:25:24
REC_SNAPSHOT_END|TOTAL=3
REC_STORAGE|USED_BYTES=2147483648|TOTAL_BYTES=128034708480|AVAILABLE_BYTES=85731590144|FILE_COUNT=3
```

### 2-2. 연결 유지 중 실시간 목록 갱신

- 새 1분 세그먼트 저장 완료 후 DB 반영이 끝나면:

```text
REC_ADD|<id>|<created_at>
REC_STORAGE|USED_BYTES=<n>|TOTAL_BYTES=<n>|AVAILABLE_BYTES=<n>|FILE_COUNT=<n>
```

- 오래된 녹화가 cleanup으로 삭제되면:

```text
REC_DEL|<id>
REC_STORAGE|USED_BYTES=<n>|TOTAL_BYTES=<n>|AVAILABLE_BYTES=<n>|FILE_COUNT=<n>
```

예시:

```text
REC_ADD|3625|2026-03-24T16:28:24
REC_STORAGE|USED_BYTES=2214592512|TOTAL_BYTES=128034708480|AVAILABLE_BYTES=85664473088|FILE_COUNT=4
REC_DEL|3511
REC_STORAGE|USED_BYTES=2147483648|TOTAL_BYTES=128034708480|AVAILABLE_BYTES=85731590144|FILE_COUNT=3
```

스토리지 필드 설명:
- `USED_BYTES`: 현재 `videos` 폴더에 존재하는 녹화 파일 총합 바이트
- `TOTAL_BYTES`: 서버 파일시스템 전체 용량 바이트
- `AVAILABLE_BYTES`: 서버 파일시스템 가용 용량 바이트
- `FILE_COUNT`: 현재 `videos` 폴더에서 Video Catalog에 잡히는 파일 개수

### 2-3. 클라이언트 재생 요청

- 사용자가 목록에서 항목을 고르면 `id`만 다시 서버로 보냅니다.

형식:

```text
PLAY_REC|<id>
```

예시:

```text
PLAY_REC|3624
```

### 2-4. 서버 재생 응답

- 서버는 해당 `id`를 DB에서 조회한 뒤 실제 파일이 있으면 HTTP 재생 URL을 반환합니다.

형식:

```text
PLAY_URL|<id>|<created_at>|<url>
```

예시:

```text
PLAY_URL|3624|2026-03-24T16:27:24|http://192.168.0.101:8080/videos/rec_20260324_162724.mp4
```

설명:
- `url`은 `SFEPS_VIDEO_HTTP_BASE_URL + "/" + basename(filename)` 규칙으로 생성됩니다.
- 목록 단계에서는 `filename`과 `play_url`을 보내지 않습니다.
- `filename`은 서버 내부에서 파일 존재 확인과 URL 생성용으로만 사용합니다.

### 2-5. 오류 응답

목록/연결 단계 오류:

```text
REC_ERR|<code>|<message>
```

재생 요청 오류:

```text
PLAY_ERR|<code>|<message>
```

주요 code:
- `MAX_CLIENTS`
- `INVALID_REQUEST`
- `PAYLOAD_TOO_LARGE`
- `NOT_FOUND`

예시:

```text
REC_ERR|MAX_CLIENTS|video catalog max clients reached
PLAY_ERR|INVALID_REQUEST|expected PLAY_REC|<id>
PLAY_ERR|INVALID_REQUEST|id must be positive integer
PLAY_ERR|NOT_FOUND|recording id not found
PLAY_ERR|NOT_FOUND|recording file missing
```

## 3) 접속 포트/보안

- Plain:
  - `SFEPS_VIDEO_CATALOG_PORT` (기본 `5559`)
- TLS:
  - `SFEPS_VIDEO_CATALOG_TLS_PORT` (기본 `6559`)
- allowlist:
  - 서버의 `SFEPS_ALERT_ALLOW_IPS` 정책 공유

## 4) Qt 구현 가이드

- `VideoArchiveManager`가 소켓을 1회 연결하고 연결을 유지합니다.
- 연결 직후 오는 스냅샷으로 목록 모델을 초기화합니다.
- 목록 모델 role은 최소 `id`, `createdAt`만 있으면 됩니다.
- `REC_ADD` 수신 시 새 항목을 목록에 추가합니다.
- `REC_DEL` 수신 시 해당 `id` 항목을 목록에서 제거합니다.
- `REC_STORAGE` 수신 시 저장공간 UI를 갱신합니다.
- 사용자가 항목을 선택하면 `PLAY_REC|<id>\n` 전송 후 `PLAY_URL`을 기다립니다.
- `PLAY_URL` 수신 시 마지막 필드의 URL을 `QMediaPlayer` 또는 QML `MediaPlayer.source`에 넣어 재생합니다.

권장 UI 흐름:
1. 앱 진입 시 Video Catalog 연결
2. `REC_SNAPSHOT_BEGIN` 수신 시 목록 초기화 시작
3. `REC` 수신 시 항목 누적
4. `REC_SNAPSHOT_END` 수신 시 초기 로딩 종료
5. `REC_STORAGE` 수신 시 저장공간 표시 갱신
6. 사용자가 목록 선택
7. `PLAY_REC|id` 전송
8. `PLAY_URL` 수신 후 재생

## 5) 검증 시나리오

1. 초기 연결
- 연결 직후 `REC_SNAPSHOT_BEGIN -> REC... -> REC_SNAPSHOT_END` 순서로 전체 목록 수신

2. 실시간 반영
- 새 1분 세그먼트 저장 완료 후 `REC_ADD|id|created_at` 수신

3. 삭제 반영
- cleanup로 오래된 파일 삭제 후 `REC_DEL|id` 수신
- 직후 `REC_STORAGE|...`로 용량 정보 갱신

4. 정상 재생
- `PLAY_REC|<valid_id>` -> `PLAY_URL|id|created_at|url`

5. 잘못된 재생 요청
- `PLAY_REC|abc` -> `PLAY_ERR|INVALID_REQUEST|...`

6. 존재하지 않는 항목
- `PLAY_REC|99999999` -> `PLAY_ERR|NOT_FOUND|...`

7. HTTP 재생
- `PLAY_URL`의 `http://<host>:8080/videos/<file>.mp4`가 실제 재생 가능
- seek가 필요하므로 운영 HTTP 서버는 `Range` 요청을 지원해야 함

## 6) 운영 참고

- 새 영상 목록 최신화는 “녹화 중간”이 아니라 “1분 세그먼트 저장 완료 후” 반영됩니다.
- 실제 MP4 바이트 전송은 앱 서버가 아니라 운영 HTTP `/videos` 정적 서빙이 담당합니다.
- 운영에서 `http://<host>:8080/videos/<filename>`가 `/home/iam/SFEPS/videos/<filename>`로 매핑되어 있어야 합니다.
