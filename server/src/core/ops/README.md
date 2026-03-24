# ops

이 폴더는 서버가 계속 실행되는 동안 필요한 운영성 작업을 담당합니다. 핵심은 "로그 적재"와 "오래된 파일 정리"입니다.

## 파일

### `cleanup.cpp`
- `run_file_cleanup_worker`
  - `rec_*.mp4` 형식의 녹화 파일을 주기적으로 스캔해 보관 기간이 지난 파일을 삭제합니다.
- `run_fraud_image_cleanup_worker`
  - Fraud 이미지 디렉토리에서 오래된 `.jpg`/`.jpeg` 파일을 삭제합니다.
- `run_pending_image_cleanup_worker`
  - Pending 이미지 디렉토리에서 오래된 `.jpg`/`.jpeg` 파일을 삭제합니다.
- 내부 공통 함수 `run_image_retention_cleanup_worker`가 이미지 정리 로직을 공유합니다.
- 워커들은 10초 주기로 돌고, 종료 신호를 빨리 반영하기 위해 200ms 단위로 나눠 잠듭니다.

### `log.cpp`
- `DBLogger`를 구현합니다.
- 역할:
  - 로그인 성공/실패 로그를 `login_logs`에 적재
  - 녹화 파일 로그를 `recordings`에 적재
  - 오래된 `analytics_logs`, `login_logs`, `recordings` 정리 요청 처리
- 특징:
  - 메인 스레드가 직접 DB에 쓰지 않고 큐 + 워커 스레드로 비동기 처리합니다.
  - MySQL prepared statement를 재사용해 반복 INSERT 비용을 줄입니다.
  - `recordings.created_at` 컬럼이 없을 가능성까지 고려해 파일명 기반 정리 쿼리로 fallback 합니다.

## `main.cpp`와의 연결

- 서버 기동 직후 `DBLogger::connect()`로 로거를 시작합니다.
- 녹화/인증 흐름에서 `enqueueRecording`, `enqueueLogin`이 호출됩니다.
- 별도 스레드가 60초마다 `requestDbCleanup()`를 호출합니다.
- 파일 정리 워커는 서버 종료 시 `g_running=false`를 감지하고 순차적으로 빠져나옵니다.
