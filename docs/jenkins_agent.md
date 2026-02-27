Jenkins agent Docker image for SFEPS CI
=====================================

This document explains how to build and use the provided Jenkins agent image which includes system packages required to build and run the SFEPS server inside CI.

Build locally
-------------

1. Build image on the machine that can run Docker (host or CI builder):

```bash
./scripts/build_agent_image.sh sfeps-jenkins-agent:latest
```

2. Optionally push to a registry accessible by your Jenkins instance:

```bash
docker tag sfeps-jenkins-agent:latest my-registry.example.com/myorg/sfeps-jenkins-agent:latest
docker push my-registry.example.com/myorg/sfeps-jenkins-agent:latest
```

Use in Jenkinsfile
------------------

If your Jenkins has the Docker Pipeline plugin and can run Docker, set the pipeline agent to use the image:

```groovy
pipeline {
  agent { docker { image 'my-registry.example.com/myorg/sfeps-jenkins-agent:latest' } }
  stages { /* ... */ }
}
```

Notes
-----
- If your Jenkins master is itself a Docker container, building images inside that container requires Docker-in-Docker or access to the host Docker socket. Alternatively build the image on the host and push to a registry.
- The image runs as user `jenkins` (non-root). Certain operations (like apt install) are not performed in the container at runtime; the image includes the packages already installed.
- If your environment restricts running Docker, install the packages from `scripts/install_agent_deps.sh` on the agent instead.
