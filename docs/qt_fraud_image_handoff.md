# Qt 전달용: 서버 부정승차 이미지 수신 스펙 (IMG_REF)

최종 갱신: 2026-03-19

## 1) 목적

- 기존 텍스트 알림(`FRAUD|...`)은 유지
- 서버가 추가로 보내는 `IMG_REF|...`를 받아 이미지 다운로드/저장/표시
- 이미지 바이너리를 소켓으로 직접 받지 않고 URL 참조 방식 사용

## 2) 서버 송신 채널

- Alert 소켓(기본 평문): `tcp://<server_ip>:5557`
- Alert TLS 소켓(옵션): `tcp://<server_ip>:6557`
- 전송 단위: `\n`(개행)으로 구분되는 라인 메시지

참고:
- 기존과 동일하게 Alert 채널 브로드캐스트로 전송됨
- `FRAUD` 라인과 `IMG_REF` 라인의 도착 순서는 완전히 고정되지 않을 수 있음

## 3) 수신 메시지 포맷

### 3-1. 기존 FRAUD (유지)

예시:
`FRAUD|423219|Adult|Senior|Y|L=12.3|T=45.6|R=78.9|B=101.1|X=50.0|Y=60.0|TAG=2026-03-19T10:11:12.123Z`

핵심 필드:
- `object_id`: 2번째 토큰
- `TAG`: `TAG=<iso8601>` 형태의 토큰

### 3-2. 신규 IMG_REF (추가)

예시:
`IMG_REF|OBJECT_ID=423219|URL=http://192.168.0.10:8080/fraud-images/rfid_1773890000123_423219.jpg|TAG=2026-03-19T10:11:12.123Z|NAME=rfid_1773890000123_423219.jpg`

필드 정의:
- `OBJECT_ID`: 부정승차 객체 ID
- `URL`: JPG 다운로드 URL
- `TAG`: 서버 판정 시각(ISO-8601)
- `NAME`: 서버 파일명

## 4) Qt 구현 순서 (담당자 작업 지시용)

1. Alert 소켓에서 라인 단위 수신
2. prefix가 `FRAUD|`이면 기존 로직대로 텍스트 이벤트 처리
3. prefix가 `IMG_REF|`이면 키=값 파싱
4. `OBJECT_ID + TAG`를 이벤트 키로 사용해 내부 모델 매칭
5. `URL`을 `QNetworkAccessManager`로 비동기 다운로드
6. 성공 시 로컬 파일 저장
7. 저장된 로컬 파일 경로(`file:///...`)를 UI 모델에 반영해 즉시 표시
8. 실패 시 텍스트 이벤트는 유지하고 이미지 상태만 `failed`로 표시

저장 경로 권장:
- `QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/fraud_images/"`

파일명 권장:
- 우선순위 1: `IMG_REF.NAME` 그대로 사용
- 우선순위 2(대체): `fraud_<OBJECT_ID>_<epoch_ms>.jpg`

## 5) 매칭/중복/순서 역전 처리

- 키: `event_key = OBJECT_ID + "|" + TAG`
- `FRAUD` 먼저 오면 텍스트 먼저 표시하고 이미지 `pending`
- `IMG_REF` 먼저 오면 임시로 이벤트를 생성하거나 보류 후 `FRAUD` 도착 시 병합
- 재연결 등으로 중복 수신될 수 있으므로 `event_key` 기준 dedupe 필요

## 6) 다운로드 실패 기준 및 재시도 권장

- 실패 조건:
  - HTTP status != 200
  - timeout
  - 응답 바디 0 byte
  - JPG 디코드 실패
- 재시도:
  - 1~2회(짧은 backoff) 권장
- 최종 실패:
  - UI에 "이미지 수신 실패" 표시
  - 텍스트(`FRAUD`) 알림은 유지

## 7) 운영 전제(서버 측 완료/필수 확인)

- `IMG_REF.URL`은 서버 env `SFEPS_FRAUD_IMAGE_HTTP_BASE_URL` 기준으로 생성됨
- 정적 매핑:
  - `http://<host>:8080/fraud-images/<filename>`
  - `/home/iam/SFEPS/event_images/fraud/<filename>`
- 서버 보관 정책:
  - `SFEPS_FRAUD_IMAGE_RETENTION_SEC` 기본 `86400`(1일)

## 8) Qt 검증 체크리스트

1. 부정승차 1건 발생 시 `FRAUD`와 `IMG_REF` 모두 수신되는지
2. `IMG_REF.URL` 다운로드 성공 후 로컬 저장되는지
3. 상세/모니터링 화면에서 저장 파일이 즉시 표시되는지
4. 네트워크 끊김 시 텍스트는 유지되고 이미지 실패 처리되는지
5. 재연결 후 중복 이벤트가 1건으로 정리되는지
