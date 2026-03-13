# Jenkins CD Setup (develop -> test, main -> prod)

This project pipeline supports:

1. `develop`: run CI tests -> build/push ARM Docker image -> deploy to test Raspberry Pi
2. `main`: run CI tests -> build/push ARM Docker image -> manual approval -> deploy to production Raspberry Pi

## Required Jenkins Credentials

- `sfeps-registry-creds` (Username/Password):
  - Private registry login for image push/pull
- `sfeps-test-ssh` (SSH username + private key):
  - SSH access to test Raspberry Pi
- `sfeps-prod-ssh` (SSH username + private key):
  - SSH access to production Raspberry Pi

Credential IDs are configurable in Jenkins env:

- `SFEPS_REGISTRY_CREDENTIALS_ID`
- `SFEPS_TEST_SSH_CREDENTIALS_ID`
- `SFEPS_PROD_SSH_CREDENTIALS_ID`

## Required Jenkins Environment Variables

- `SFEPS_DOCKER_REGISTRY` (required for CD): e.g. `registry.example.com`
- `SFEPS_DOCKER_IMAGE_REPO` (default: `sfeps/server`)
- `SFEPS_DOCKER_PLATFORM` (default: `linux/arm64`)
- `SFEPS_DOCKERFILE_PATH` (default: `docker/server/Dockerfile`)

- `SFEPS_TEST_HOST` (required for `develop` deploy)
- `SFEPS_PROD_HOST` (required for `main` deploy)

Optional deployment tuning:

- `SFEPS_TEST_CONTAINER_NAME` (default: `sfeps-server-test`)
- `SFEPS_PROD_CONTAINER_NAME` (default: `sfeps-server-prod`)
- `SFEPS_REMOTE_ENV_FILE` (default: `/home/iam/SFEPS/server/.env.local`)
- `SFEPS_CONTAINER_ENV_FILE` (default: `/opt/sfeps/server/.env.local`)
- `SFEPS_REMOTE_PKI_DIR` (default: `/etc/sfeps/pki`)
- `SFEPS_VIDEO_DIR` (default: `/home/iam/SFEPS/videos`)
- `SFEPS_HEALTH_PORT` (default: `5555`)

## Agent Prerequisites

Jenkins execution node needs:

- Docker with Buildx support
- SSH client

Remote Raspberry Pis need:

- Docker runtime
- Network access to private registry
- Local files expected by server:
  - env file (`SFEPS_REMOTE_ENV_FILE`)
  - PKI directory (`SFEPS_REMOTE_PKI_DIR`)

## Deployment Behavior

Remote deployment sequence:

1. `docker login` to registry
2. `docker pull <image>`
3. `docker rm -f <container> || true`
4. `docker run -d --network host ...`
5. health check on `127.0.0.1:${SFEPS_HEALTH_PORT}`
