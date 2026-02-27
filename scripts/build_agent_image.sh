#!/usr/bin/env bash
set -euo pipefail

IMAGE_NAME=${1:-sfeps-jenkins-agent:latest}
DOCKER_DIR="docker/jenkins-agent"

echo "Building agent image: ${IMAGE_NAME}"
docker build -t "${IMAGE_NAME}" "${DOCKER_DIR}"

echo "Built ${IMAGE_NAME}"
echo "To push: docker tag ${IMAGE_NAME} <registry>/
<repo>:tag && docker push <registry>/<repo>:tag"
