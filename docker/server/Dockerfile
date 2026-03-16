FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    pkg-config \
    bash \
    ca-certificates \
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
 && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/sfeps

COPY server ./server
COPY hardware/Raspi-driver/Audio_Speaker_Unit ./hardware/Raspi-driver/Audio_Speaker_Unit
COPY Camera/get_metadata ./Camera/get_metadata

RUN cmake -S server -B server/build \
 && cmake --build server/build -j"$(nproc)"

WORKDIR /opt/sfeps/server

CMD ["./run_server.sh"]
