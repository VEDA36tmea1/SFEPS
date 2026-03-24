# Camera Trigger Capture Handoff

## 1. 변경 목적

이번 변경의 목표는 `RFID 이벤트 소비 주체`를 서버로 단일화하고, 카메라는 서버의 명시적 요청이 있을 때만 촬영하도록 바꾸는 것입니다.

- 기존 문제:
  - 서버와 카메라 프로세스가 동시에 RFID 소켓을 소비하면 이벤트 경합 가능성 존재
  - 촬영 결과를 고정 파일(`4_best_shot.jpg`) 중심으로 다루다 보니 이벤트 단위 추적이 어려움
- 변경 방향:
  - RFID는 서버만 수신
  - 카메라는 UDS 트리거 기반 촬영 워커로 동작
  - 촬영 결과를 요청별 경로(`OUT`)에 직접 저장

---

## 2. 변경 전/후 동작 비교

## 변경 전

- `camera_client`가 `/tmp/rc522_events.sock`를 직접 구독
- RFID 태그 발생 시 카메라 내부 큐에 프레임 적재 후 파이프라인 수행
- 결과 파일은 사실상 고정 산출물 중심(`4_best_shot.jpg`)으로 관리

## 변경 후

- `camera_client`는 RFID를 직접 읽지 않음
- 로컬 UDS 서버(`/tmp/sfeps_camera_trigger.sock`)를 열고 `CAPTURE_REQ` 대기
- 요청마다 `OUT` 절대경로에 최종 이미지 저장
- 서버 응답 없이 fire-and-forget으로 처리

---

## 3. 새 IPC 프로토콜 (서버 ↔ 카메라)

## 소켓 경로

- `/tmp/sfeps_camera_trigger.sock` (AF_UNIX, SOCK_STREAM)

## 서버 → 카메라 요청

```text
CAPTURE_REQ|REQ_ID=<req_id>|OBJECT_ID=<object_id>|TAG=<tag_utc>|OUT=<abs_path>\n
```

- `REQ_ID`: 요청 식별자
- `OBJECT_ID`: 서버에서 매칭한 객체 ID
- `TAG`: RFID 매칭 시점 태그 시간(UTC 문자열)
- `OUT`: 카메라가 저장해야 하는 절대 경로

## 4. 카메라 프로세스 내부 구조 (현재)

`camera_client`는 현재 3개 스레드 구조입니다.

1. `captureThread`
- 카메라에서 프레임 지속 수집
- 최신 프레임 1장만 유지(`g_raw_queue`)

2. `triggerListenerThread`
- `/tmp/sfeps_camera_trigger.sock`에서 `accept`
- `CAPTURE_REQ` 라인 파싱
- 필드/경로 검증
- 요청 큐에 적재

3. `pipelineWorkerThread`
- 요청 큐에서 pop
- 최신 프레임 복제
- ISP 파이프라인 실행
- `OUT` 경로 저장

큐 정책:

- 요청 큐 최대 5개
- 큐 초과 시 경고 로그 후 드롭

---

## 5. camera_client.cpp 핵심 변경 포인트

## A. RFID 소비 제거

- 기존 RFID 소켓(`/tmp/rc522_events.sock`) 구독 로직 삭제
- `rfidThreadFunc` 제거

결과:

- 카메라는 RFID 이벤트를 직접 소비하지 않고 서버 트리거만 처리

## B. 트리거 수신 서버 추가

- `kTriggerSocketPath = "/tmp/sfeps_camera_trigger.sock"`
- `triggerListenerThread()`에서 bind/listen/accept 수행
- 요청 라인 파싱 후 필수 필드 검사:
  - `REQ_ID`, `OBJECT_ID`, `TAG`, `OUT`

## C. 경로 안전성 검사 추가

`OUT` 경로 허용 조건:

- 절대경로여야 함
- `\n`, `\r`, `..` 포함 금지
- `/home/iam/SFEPS/event_images/pending/` 하위 경로만 허용

허용되지 않으면 `OUT_PATH_NOT_ALLOWED` 경고 로그를 남기고 드롭

## D. 파이프라인 출력 경로 인자화

- `runFullPipeline(frame, raw_mode, out_path, err)` 형태로 변경
- 기존 고정 저장 대신 요청별 `out_path`에 최종 저장

디버그 산출물은 기존 유지:

- `1_raw_capture.jpg`
- `2_pure_isp_out.jpg` (RAW일 때)
- `3_tuning_viewer.jpg`

## E. 요청 처리 정책

- 서버는 `CAPTURE_REQ` 전송 성공 시 바로 pending registry를 갱신
- 카메라는 별도 ACK 없이 요청을 큐에 넣고 저장만 수행
- 잘못된 요청, 큐 초과, 프레임 부재, 저장 실패는 카메라 서비스 로그로만 확인

## F. 종료 처리 개선

- SIGINT/SIGTERM 수신 시
  - `g_running=false`
  - 조건변수 notify
  - listener fd close
- 루프 종료 시 소켓 파일 unlink

---

## 6. 서버 측 연동 변경 (카메라 담당자가 알아야 하는 범위)

카메라 쪽 인터페이스와 맞물리는 서버 변경 사항입니다.

1. RFID paired 콜백 인자 확장
- `(object_id)` → `(object_id, tag_time)`

2. RFID 매칭 시 카메라 촬영 요청
- 서버가 `CAPTURE_REQ` 전송
- `connect + send` 성공 시점에 바로 pending registry 등록
- outline 시점에는 registry 우선, registry miss면 `pending` 디렉터리에서 `object_id` 기반 fallback 검색 수행

3. 파일명 규칙(서버 생성)
- `capture_<TAG>_<object_id>_<req_id>.jpg`

서버 측 로그 키:

- `CAM_TRIGGER_SEND`
- `CAM_TRIGGER_SEND_FAIL`
- `CAM_TRIGGER_REGISTRY_SET`
- `RFID_IMAGE_FALLBACK_HIT`
- `RFID_IMAGE_FALLBACK_MISS`

---

## 7. 운영상 기대 효과

1. RFID 이벤트 소비 경합 완화
- 서버만 RFID를 읽으므로 카메라와의 중복 소비 이슈를 구조적으로 방지

2. 이벤트 단위 추적성 향상
- 요청별 파일명/REQ_ID로 어떤 이벤트의 이미지인지 명확히 추적 가능

3. finalize 회복력 향상
- 늦게 저장된 pending 파일도 outline 시점 fallback 검색으로 회수 가능

---

## 8. 카메라 담당자 체크리스트

1. 프로세스 기동 후 로그 확인
- `[camera] trigger listener ready: /tmp/sfeps_camera_trigger.sock`

2. 요청 처리 흐름 확인
- 서버에서 `CAPTURE_REQ` 전송 시 카메라가 요청을 큐에 적재하고 실제 파일을 생성하는지

3. 출력 경로 확인
- `OUT`가 `/home/iam/SFEPS/event_images/pending/...` 하위로 전달되는지
- 실제 파일이 생성되는지

4. 성능/안정성 확인
- 연속 요청(2~3건)에서 큐 처리 정상 여부
- 큐 초과 시 경고 로그만 남기고 서버가 계속 동작하는지

5. 종료 처리 확인
- 종료 시 `/tmp/sfeps_camera_trigger.sock` 정리되는지

---

## 9. 참고 파일

- 카메라 구현:
  - `Camera/image_processing/src/camera_client.cpp`
- 서버 연동:
  - `server/src/modules/streaming/rfid_image_pipeline.cpp`
  - `server/src/modules/streaming/analytics.cpp`
  - `server/include/analytics.h`
  - `server/src/main.cpp`
  - `server/include/rfid_image_pipeline.h`
