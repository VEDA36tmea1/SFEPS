FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    pkg-config \
    git \
    python3 \
    python3-venv \
    python3-pip \
    python3-pytest \
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
 && rm -rf /var/lib/apt/lists/*

RUN useradd -m -s /bin/bash jenkins || true
USER jenkins
WORKDIR /home/jenkins


CMD ["/bin/bash"]
