# Jenkins CI/CD Setup (develop -> test, main -> prod)

최종 갱신: 2026-03-19

현재 `Jenkinsfile` 기준 파이프라인 동작:
1. 공통 CI(빌드 + 테스트)
2. `develop`/`main`에서 ARM 이미지 빌드/푸시
3. `develop`이면 test Raspberry 자동 배포
4. `main`이면 승인 후 prod Raspberry 배포
5. post 단계에서 리포트 생성/아카이브 + 선택적 Slack 알림

## Pipeline Stages

1. Checkout
2. Resolve CI Metadata (브랜치/sha/이미지태그 계산)
3. Build Server
4. Check Test Tooling
5. Start Local MariaDB
6. Run Login Tests
7. Start Local MediaMTX
8. Start RTSP Publisher
9. Run Stream Tests
10. Run Event Tests
11. Build & Push ARM Image (`develop`/`main`)
12. Deploy To Test Raspberry (`develop`)
13. Approve Production Deployment (`main`)
14. Deploy To Production Raspberry (`main`)

## Required Jenkins Credentials

필수:
- `sfeps-registry-creds` (Username/Password)
- `sfeps-test-ssh` (SSH username + private key)
- `sfeps-prod-ssh` (SSH username + private key)

선택:
- `sfeps-slack-webhook` (Slack webhook URL, `SFEPS_SLACK_NOTIFY=1`일 때)

Credential ID는 환경변수로 오버라이드할 수 있습니다.
- `SFEPS_REGISTRY_CREDENTIALS_ID`
- `SFEPS_TEST_SSH_CREDENTIALS_ID`
- `SFEPS_PROD_SSH_CREDENTIALS_ID`
- `SFEPS_SLACK_WEBHOOK_CREDENTIALS_ID`

## Key Jenkins Environment Variables

이미지/빌드:
- `SFEPS_DOCKER_REGISTRY` (default: `ghcr.io`)
- `SFEPS_DOCKER_IMAGE_REPO` (default: `veda36tmea1/sfeps-server`)
- `SFEPS_DOCKER_PLATFORM` (default: `linux/arm64`)
- `SFEPS_DOCKERFILE_PATH` (default: `docker/server/Dockerfile`)

브랜치 해석 보정:
- `SFEPS_SINGLE_JOB_BRANCH` (default: `develop`)

배포 대상:
- `SFEPS_TEST_HOST` (test 배포 필수)
- `SFEPS_PROD_HOST` (prod 배포 필수)

원격 런타임 파일/마운트:
- `SFEPS_REMOTE_ENV_FILE` (default: `/home/iam/SFEPS/server/.env.local`)
- `SFEPS_CONTAINER_ENV_FILE` (default: `/opt/sfeps/server/.env.local`)
- `SFEPS_REMOTE_PKI_DIR` (default: `/etc/sfeps/pki`)
- `SFEPS_REMOTE_MYSQL_SOCK_DIR` (default: `/run/mysqld`)
- `SFEPS_VIDEO_DIR` (default: `/home/iam/SFEPS/videos`)
- `SFEPS_HEALTH_PORT` (default: `5555`)

Slack:
- `SFEPS_SLACK_NOTIFY` (default: `1`)
- `SFEPS_SLACK_CHANNEL` (optional)

## Agent/Runner Prerequisites

Jenkins 실행 노드:
- Docker + Buildx
- Python3 + pytest
- MariaDB binaries (`mariadb-install-db`, `mariadbd`, `mariadb`)
- ffmpeg
- mediamtx 실행 스크립트(`SFEPS_CI_MTX_SCRIPT`, default `/usr/local/bin/run_mediamtx_ci.sh`)

원격 Raspberry:
- Docker runtime
- registry pull 가능
- `SFEPS_REMOTE_ENV_FILE` 존재
- `SFEPS_REMOTE_MYSQL_SOCK_DIR/mysqld.sock` 존재

## Deploy Behavior (Current)

Test deploy (`develop`):
- 컨테이너 실행 전 env 파일을 필터링/재작성
- 아래 값 강제:
  - `SFEPS_APP_BIND_IP=0.0.0.0`
  - `SFEPS_APP_TLS_ENABLE=0`
  - `SFEPS_APP_PLAINTEXT_ENABLE=1`
  - `SFEPS_ESP_TCP_ENABLE=0`

Prod deploy (`main`):
- 승인 단계 후 배포
- 현재 prod도 배포 시 아래 값 강제:
  - `SFEPS_APP_TLS_ENABLE=0`
  - `SFEPS_APP_PLAINTEXT_ENABLE=1`

즉, 현재 CD 경로는 test/prod 모두 plain 모드 강제 배포입니다.

## Health Check

배포 후 원격에서 `127.0.0.1:${SFEPS_HEALTH_PORT}` TCP 오픈을 최대 90초 대기 확인합니다.

## Post Actions

- CI 임시 프로세스 정리(MariaDB, MediaMTX, ffmpeg)
- `scripts/generate_test_reports.py` 실행(실패 시 non-fatal)
- junit/xml 및 html/pdf/xls/xlsx 리포트 아카이브
- Slack 알림(옵션)
