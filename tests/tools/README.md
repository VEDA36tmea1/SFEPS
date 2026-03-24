# tests/tools - Web UI Runner

`tests/tools/test_ui_server.py`는 Jenkins API를 호출해 테스트 잡을 실행하고,
마지막 빌드 상태와 리포트 링크를 조회하는 경량 Web UI 서버입니다.

현재 UI는 파이프라인을 두 종류로 분리합니다.

- `CI` : `Jenkinsfile` 기준
- `NF` : `Jenkinsfile.nf` 기준

## 1) 실행

```bash
cd /home/iam/SFEPS
python3 tests/tools/test_ui_server.py
```

기본 접속 주소:

- `http://127.0.0.1:8787` (원격 접속 시 `http://<host-ip>:8787`)

## 2) 내장 기본값

기본값은 `test_ui_server.py`에 내장되어 있어, export 없이도 바로 실행할 수 있습니다.

- `SFEPS_TEST_UI_HOST=0.0.0.0`
- `SFEPS_TEST_UI_PORT=8787`
- `SFEPS_JENKINS_URL=http://192.168.0.88:8080`
- `SFEPS_JENKINS_JOB_CI=SFEPS`
- `SFEPS_JENKINS_JOB_NF=SFEPS-NF`
- `SFEPS_JENKINS_USER=`
- `SFEPS_JENKINS_TOKEN=`
- `SFEPS_JENKINS_VERIFY_SSL=1`

호환용(선택):

- `SFEPS_JENKINS_JOB`만 설정하면 CI/NF 양쪽 잡 이름으로 공통 사용됩니다.

## 3) 오버라이드 예시

```bash
export SFEPS_JENKINS_URL="http://jenkins.local:8080"
export SFEPS_JENKINS_JOB_CI="Folder/SFEPS-CI"
export SFEPS_JENKINS_JOB_NF="Folder/SFEPS-NF"
export SFEPS_JENKINS_USER="jenkins-user"
export SFEPS_JENKINS_TOKEN="<api-token>"
python3 tests/tools/test_ui_server.py
```

## 4) Jenkins 파라미터 매핑

### CI (Jenkinsfile)

- 트리거: `POST /build`
- 전달 파라미터: 없음

`Jenkinsfile`에는 `parameters {}` 정의가 없으므로, CI 실행은 파라미터 없이 호출합니다.

### NF (Jenkinsfile.nf)

- 트리거: `POST /buildWithParameters`
- 전달 파라미터(기본값):
  - `PERF_EVENT_TARGET_COUNT=50`
  - `PERF_EVENT_WINDOW_SECONDS=60`
  - `PERF_EVENT_INTERVAL_SECONDS=0`
  - `PERF_EVENT_MAX_MISSING=0`
  - `PERF_EVENT_MAX_DUPLICATES=0`
  - `RELI_STREAM_DURATION_SECONDS=3600`
  - `RELI_STREAM_POLL_INTERVAL_SECONDS=1`
  - `RELI_STREAM_RECOVERY_TIMEOUT_SECONDS=10`

## 5) 제공 API

- `GET /` : Web UI
- `GET /api/health` : 서버/설정 상태 확인
- `POST /api/jenkins/build` : Jenkins 빌드 트리거 (`pipeline=ci|nf`)
- `GET /api/jenkins/last-build?pipeline=ci|nf` : 마지막 빌드 상태 + `report_links` 조회

`report_links`는 Jenkins `lastBuild` 기준으로 아래 링크를 수집합니다.

- Build URL
- Console URL
- JUnit Test Report URL(존재 시)
- 아카이브된 artifact URL들(존재 시)
