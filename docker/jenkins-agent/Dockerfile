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
    openjdk-17-jre-headless \
    libmariadb-dev \
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

ENV PATH="/home/jenkins/.local/bin:${PATH}"

CMD ["/bin/bash"]
