# GStreamer laserdetect 플러그인

`rtsp_laser_demo`의 레이저 탐지 로직을 GStreamer **요소**로 쓸 수 있게 한 플러그인입니다.  
`gst-launch-1.0` 파이프라인 **안**에 `laserdetect`를 넣어서 사용합니다.

## 빌드

상위 디렉터리에서:

```bash
cd build && cmake .. && make
```

플러그인 `.so`는 `build/gst-plugin/gstlaserdetect.so`에 생성됩니다.

## 사용법

1. 플러그인 경로 지정:

```bash
export GST_PLUGIN_PATH="$(pwd)/build/gst-plugin"
# 예: /home/ros2man/Desktop/SFEPS/hardware/stm32-laser/host_cpp 에서
# export GST_PLUGIN_PATH="/home/ros2man/Desktop/SFEPS/hardware/stm32-laser/host_cpp/build/gst-plugin"
```

2. 파이프라인에서 `! laserdetect !` 로 실행:

```bash
gst-launch-1.0 \
  rtspsrc location=rtsp://admin:CCgbdCCgbd@192.168.0.84/profile2/media.smp protocols=udp latency=100 ! \
  rtph264depay ! h264parse ! avdec_h264 ! \
  videoconvert ! video/x-raw,format=BGR ! \
  laserdetect ! \
  videoconvert ! autovideosink
```

- `laserdetect`는 **BGR** 입력을 받아 레이저 점을 탐지하고, 탐지된 위치에 빨간 원을 그린 뒤 그대로 다음 요소로 넘깁니다.
- 화면 출력은 `autovideosink`(또는 `xvimagesink` 등)가 담당합니다.

## 요약

| 이전 (rtsp_laser_demo 실행 파일) | 현재 (플러그인) |
|----------------------------------|------------------|
| `./rtsp_laser_demo --gst-launch "… ! appsink"` | `gst-launch-1.0 … ! laserdetect ! autovideosink` |
| 앱이 파이프라인 문자열을 받아 appsink로 프레임 수신 | 파이프라인 **안**에 `laserdetect` 요소 삽입 |
