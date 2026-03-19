# Jenkins Agent Image for SFEPS CI

Last updated: 2026-03-19

SFEPS CI에서 서버 빌드/테스트를 수행하기 위한 Jenkins Agent Docker 이미지 사용 가이드입니다.

## Build Locally

```bash
./scripts/build_agent_image.sh sfeps-jenkins-agent:latest
```

## Push (Optional)

```bash
docker tag sfeps-jenkins-agent:latest my-registry.example.com/myorg/sfeps-jenkins-agent:latest
docker push my-registry.example.com/myorg/sfeps-jenkins-agent:latest
```

## Use in Jenkinsfile

```groovy
pipeline {
  agent {
    docker {
      image 'my-registry.example.com/myorg/sfeps-jenkins-agent:latest'
    }
  }
  stages { /* ... */ }
}
```

## Included Tooling (Dockerfile 기준)

- build: `build-essential`, `cmake`, `pkg-config`, `git`
- python: `python3`, `python3-venv`, `python3-pip`, `python3-pytest`
- db/libs: `libmariadb-dev`, `default-libmysqlclient-dev`, `libtinyxml2-dev`
- media/audio/ssl: `libavcodec-dev`, `libavformat-dev`, `libavutil-dev`, `libswscale-dev`, `libasound2-dev`, `libssl-dev`
- report: `wkhtmltopdf`

이미지는 `jenkins` 비루트 사용자로 실행됩니다.

## Notes

- Docker Pipeline 사용 환경이 아니면, 에이전트 호스트에 직접 의존성을 설치할 수 있습니다.
  - 설치 스크립트: `scripts/install_agent_deps.sh`
- Jenkins가 컨테이너 안에서 동작하는 경우, 이미지 빌드/푸시를 위해 Docker socket 또는 DinD 구성이 필요합니다.
