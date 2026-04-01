# streaming

이 폴더는 카메라 메타데이터, RFID 이벤트, 영상 파일, fraud 이미지까지 이어지는 핵심 스트리밍 파이프라인을 담당합니다. 서버에서 가장 상태가 많고, 다른 모듈과 연결도 가장 많은 영역입니다.

## 파일

### `analytics.cpp`
- `AnalyticsProcessor`를 구현합니다.
- 입력:
  - Recorder가 넘겨주는 XML 메타데이터
  - RFID monitor가 넘겨주는 카드 연령 텍스트
- 주요 역할:
  - 사람 객체 위치/바운딩 박스 최신 상태 유지
  - enter/outline 이벤트를 기반으로 pending 객체 추적
  - RFID 연령 정보와 카메라 추정 연령을 비교해 fraud 여부 판단
  - `analytics_logs` DB 적재
  - `FRAUD|...` Alert 메시지 생성
  - 이미지 후처리용 callback 호출
- 환경변수로 큐 크기, pending TTL, drop log 샘플링, rule 이름을 조정합니다.

### `recorder.cpp`
- `RTSPRecorder`를 구현합니다.
- RTSP 비디오 스트림을 1분 단위 `rec_YYYYMMDD_HHMMSS.mp4` 파일로 저장합니다.
- ONVIF 메타데이터 스트림(XML)을 재조립해 `AnalyticsProcessor::publishRaw()`로 전달합니다.
- 메타데이터 패킷 크기/문서 크기/연속 이상치 제한을 환경변수로 제어합니다.
- 녹화 파일 저장 완료 시 `DBLogger::enqueueRecording()`으로 DB 기록을 남깁니다.

### `rfid_monitor.cpp`
- `/tmp/rc522_events.sock` 유닉스 소켓에 연결해 RFID 데몬 이벤트를 읽습니다.
- NDJSON 라인을 단순 파서로 해석해 `id`, `text`, `timestamp` 등을 뽑습니다.
- 실제 분석 계층에는 `card_age_text`를 `AnalyticsProcessor::onRfidRead()`로 전달합니다.
- 연결이 끊기면 1초 간격으로 재접속을 시도합니다.

### `rfid_image_pipeline.cpp`
- RFID 매칭 시점의 이미지를 파일로 보존하는 파이프라인입니다.
- `ensure_runtime_media_dirs`
  - 영상/이벤트 이미지 디렉터리를 보장합니다.
- `snapshot_rfid_image_for_object`
  - 베스트샷 이미지를 pending 디렉터리로 복사합니다.
- `finalize_outline_image_for_object`
  - fraud면 `fraud/`로, 아니면 삭제 또는 `failed/`로 이동합니다.
- 최종 fraud 이미지의 파일명/경로를 `main.cpp`가 Alert URL 생성에 사용합니다.

## 주요 흐름

1. `recorder.cpp`가 RTSP 비디오와 XML 메타데이터를 읽습니다.
2. `analytics.cpp`가 객체 위치와 rule 이벤트를 모읍니다.
3. `rfid_monitor.cpp`가 RFID 연령 텍스트를 넣어 pending 객체와 매칭합니다.
4. outline 이벤트 시 fraud 여부를 최종 결정합니다.
5. 결과는 DB, Alert, fraud 이미지 후처리로 퍼집니다.

## 주의 포인트

- 이 폴더는 큐, TTL, drop 정책이 많아서 환경변수 기본값을 함께 보는 게 좋습니다.
- object id를 중심으로 여러 상태가 연결되므로, 포맷 변경 시 `analytics.cpp`부터 먼저 확인하는 편이 안전합니다.
