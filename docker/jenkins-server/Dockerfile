FROM jenkins/jenkins:lts-jdk17

USER root

ARG MEDIAMTX_VERSION=1.15.4

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Seoul
ENV JAVA_OPTS="-Duser.timezone=Asia/Seoul"
ENV JENKINS_JAVA_OPTS="-Duser.timezone=Asia/Seoul"

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    pkg-config \
    git \
    tzdata \
    python3 \
    python3-venv \
    python3-pip \
    python3-pytest \
    mariadb-server \
    mariadb-client \
    libmariadb-dev \
    libmariadb-dev-compat \
    default-libmysqlclient-dev \
    libtinyxml2-dev \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libswscale-dev \
    libasound2-dev \
    libssl-dev \
    curl \
    xz-utils \
    ca-certificates \
 && arch="$(dpkg --print-architecture)" \
 && case "$arch" in \
      arm64) mtx_arch="arm64" ;; \
      amd64) mtx_arch="amd64" ;; \
      *) echo "Unsupported architecture for mediamtx: $arch" >&2; exit 1 ;; \
    esac \
 && curl -fsSL -o /tmp/mediamtx.tar.gz "https://github.com/bluenviron/mediamtx/releases/download/v${MEDIAMTX_VERSION}/mediamtx_v${MEDIAMTX_VERSION}_linux_${mtx_arch}.tar.gz" \
 && tar -xzf /tmp/mediamtx.tar.gz -C /tmp \
 && install -m 0755 /tmp/mediamtx /usr/local/bin/mediamtx \
 && mkdir -p /etc/mediamtx \
 && echo "${TZ}" > /etc/timezone \
 && ln -snf "/usr/share/zoneinfo/${TZ}" /etc/localtime \
 && mkdir -p /home/iam/SFEPS/videos \
 && chown -R jenkins:jenkins /home/iam/SFEPS \
 && rm -rf /tmp/mediamtx /tmp/mediamtx.tar.gz \
 && rm -rf /var/lib/apt/lists/*

COPY mediamtx/mediamtx.yml /etc/mediamtx/mediamtx.yml
COPY mediamtx/run_mediamtx.sh /usr/local/bin/run_mediamtx_ci.sh
RUN chmod +x /usr/local/bin/run_mediamtx_ci.sh

USER jenkins
