# Qt 전달용: 서버 녹화영상 찾아보기(Video Catalog) 연동 안내

최종 갱신: 2026-03-19

## 1) 목적

- Qt 클라이언트에서 서버 녹화영상을 검색/조회/선택 재생
- 서버는 Video Catalog Plain/TLS 채널에서 목록 제공
- 재생 URL은 `play_url`(HTTP MP4)

## 2) 서버 요청/응답 프로토콜

### 2-1. 요청

- 한 줄 텍스트(개행 `\n` 종료)
- 명령:
  - `LIST_REC|FROM=<value>|TO=<value>|Q=<value>|PAGE=<n>|SIZE=<n>\n`

예시:

```text
LIST_REC|PAGE=1|SIZE=20
LIST_REC|FROM=2026-03-18 00:00:00|TO=2026-03-18 23:59:59|Q=rec_20260318|PAGE=1|SIZE=30
```

파라미터 규칙:
- `PAGE`: 1 이상 정수
- `SIZE`: 1~100 정수
- `FROM`, `TO`, `Q`: 선택
- 요청 총 길이 최대: 4096 bytes

### 2-2. 정상 응답

- 레코드 여러 줄:
  - `REC|<id>|<created_at>|<duration_sec>|<play_url>`
- 종료 1줄:
  - `REC_END|PAGE=<n>|SIZE=<n>|TOTAL=<n>|HAS_NEXT=<0|1>`

응답 필드:
- `id`: `recordings.id`
- `created_at`: ISO8601 (`YYYY-MM-DDTHH:MM:SS`)
- `duration_sec`: 현재 `0` 고정
- `play_url`: `SFEPS_VIDEO_HTTP_BASE_URL + "/" + basename(filename)`

예시:

```text
REC|3622|2026-03-18T15:05:45|0|http://192.168.0.101:8080/videos/rec_20260318_150458.mp4
REC|3621|2026-03-18T15:04:58|0|http://192.168.0.101:8080/videos/rec_20260318_150358.mp4
REC_END|PAGE=1|SIZE=2|TOTAL=455|HAS_NEXT=1
```

### 2-3. 오류 응답

형식:
- `REC_ERR|<code>|<message>`

주요 code:
- `INVALID_REQUEST`
- `DB_UNAVAILABLE`
- `DB_ERROR`
- `PAYLOAD_TOO_LARGE`
- `MAX_CLIENTS`

예시:

```text
REC_ERR|INVALID_REQUEST|PAGE must be integer >= 1
REC_ERR|INVALID_REQUEST|SIZE must be integer in range 1..100
REC_ERR|PAYLOAD_TOO_LARGE|request exceeds maximum size
REC_ERR|MAX_CLIENTS|video catalog max clients reached
REC_ERR|DB_UNAVAILABLE|database connection failed
```

## 3) 접속 포트/보안

- Plain:
  - `SFEPS_VIDEO_CATALOG_PORT` (기본 `5559`)
- TLS:
  - `SFEPS_VIDEO_CATALOG_TLS_PORT` (기본 `6559`)
- allowlist:
  - 서버의 `SFEPS_ALERT_ALLOW_IPS` 정책 공유

## 4) Qt 구현 가이드

- 신규 매니저(`VideoArchiveManager`)에서 소켓 연결/요청/파싱
- 목록 모델(`RecordingListModel`) role:
  - `id`, `createdAt`, `durationSec`, `playUrl`
- 화면(`ArchiveView`)에서:
  - 검색/필터 입력
  - 페이지 이동(이전/다음)
  - 목록 선택 시 `playUrl`을 `MediaPlayer.source`에 바인딩

필수 처리:
- `REC` 누적 append
- `REC_END` 수신 시 로딩 종료 + 페이지 상태 갱신
- `REC_ERR` 수신 시 사용자 메시지 표시

## 5) 검증 시나리오

1. 정상 목록 조회
- `LIST_REC|PAGE=1|SIZE=20` -> `REC...` + `REC_END`

2. 잘못된 요청
- `PAGE=0`, `SIZE=999` -> `REC_ERR|INVALID_REQUEST|...`

3. 과대 요청
- 4096 bytes 초과 요청 -> `REC_ERR|PAYLOAD_TOO_LARGE|...`

4. 페이지네이션
- `HAS_NEXT=1`일 때 다음 페이지 요청 가능

5. 재생
- 선택한 `play_url`로 영상 재생 가능

6. 회귀
- Auth/Audio/Alert/Position 흐름 영향 없음

## 6) 참고

- 현재 릴리스는 `duration_sec=0` 고정
- `play_url` 접근 가능 여부는 운영 HTTP `/videos` 구성 상태에 의존
