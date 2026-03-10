FROM jenkins/jenkins:lts-jdk17

USER root

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
    ca-certificates \
 && ln -snf "/usr/share/zoneinfo/${TZ}" /etc/localtime \
 && echo "${TZ}" > /etc/timezone \
 && mkdir -p /home/iam/SFEPS/videos \
 && chown -R jenkins:jenkins /home/iam/SFEPS \
 && rm -rf /var/lib/apt/lists/*

USER jenkins
