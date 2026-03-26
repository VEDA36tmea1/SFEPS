# Windows 빌드 & 실행 가이드 (camera_RBF)

## 목차
1. [필수 요구사항](#1-필수-요구사항)
2. [GStreamer 설치](#2-gstreamer-설치)
3. [OpenCV (GStreamer 포함) 빌드](#3-opencv-gstreamer-포함-빌드)
4. [경로 커스터마이징](#4-경로-커스터마이징)
5. [빌드](#5-빌드)
6. [실행](#6-실행)
7. [GStreamer vs FFmpeg 선택](#7-gstreamer-vs-ffmpeg-선택)
8. [문제 해결](#8-문제-해결)

---

## 1. 필수 요구사항

| 도구 | 버전 | 비고 |
|------|------|------|
| Visual Studio 2022 | Community 이상 | C++ 데스크톱 개발 워크로드 포함 |
| CMake | 3.16 이상 | `cmake --version` 으로 확인 |
| GStreamer MSVC | 1.x | [공식 다운로드](https://gstreamer.freedesktop.org/download/) → Windows MSVC x86_64 |
| OpenCV | 4.x (GStreamer 활성화 빌드) | 아래 빌드 방법 참고 |
| GNU make | (make.exe) | MinGW/MSYS2/Git Bash 등에서 제공 |

> **중요**: OpenCV 공식 배포판은 GStreamer가 **비활성화**되어 있으므로 직접 빌드해야 합니다.

---

## 2. GStreamer 설치

1. [https://gstreamer.freedesktop.org/download/](https://gstreamer.freedesktop.org/download/) 에서
   - **MSVC 64-bit** 런타임 패키지 설치
   - **MSVC 64-bit** 개발(development) 패키지 설치
2. 기본 설치 경로: `C:\Program Files\gstreamer\1.0\msvc_x86_64`

설치 확인:
```powershell
& "C:\Program Files\gstreamer\1.0\msvc_x86_64\bin\gst-launch-1.0.exe" --version
```

---

## 3. OpenCV (GStreamer 포함) 빌드

**VS 2022 Developer PowerShell** 에서 실행:

```powershell
# OpenCV 소스 위치로 이동 (없으면 git clone 먼저)
# git clone https://github.com/opencv/opencv.git C:\opencv\sources

cd C:\opencv  # 소스가 있는 상위 폴더
mkdir build_gst -ErrorAction SilentlyContinue
cd build_gst

cmake -G "Visual Studio 17 2022" -A x64 `
  -D CMAKE_BUILD_TYPE=Release `
  -D CMAKE_INSTALL_PREFIX="C:/opencv-gst/install" `
  -D BUILD_LIST=core,highgui,imgproc,videoio,imgcodecs `
  -D BUILD_opencv_world=ON `
  -D WITH_GSTREAMER=ON `
  -D WITH_FFMPEG=ON `
  -D CMAKE_PREFIX_PATH="C:/Program Files/gstreamer/1.0/msvc_x86_64" `
  ../sources

cmake --build . --config Release --target INSTALL -j 8
```

> `CMAKE_INSTALL_PREFIX` 는 원하는 경로로 자유롭게 변경 가능.

빌드 성공 확인 (cmake 출력에서):
```
Video I/O:
  GStreamer:                   YES (...)
  FFMPEG:                      YES (...)
```

---

## 4. 경로 커스터마이징

OpenCV나 GStreamer가 기본 경로가 아닌 곳에 있으면 `Makefile` 상단 두 줄만 변경:

```makefile
OPENCV_ROOT    ?= C:/opencv-gst/install          # ← OpenCV 설치 루트 변경
GSTREAMER_ROOT ?= C:/Program Files/gstreamer/1.0/msvc_x86_64  # ← GStreamer 루트 변경
```

또는 빌드 명령에서 덮어쓰기:

```powershell
.\make_msvc.cmd rbf OPENCV_ROOT="D:/libs/opencv/install"
.\make_msvc.cmd rbf GSTREAMER_ROOT="D:/libs/gstreamer/msvc_x86_64"
```

OpenCV DLL 이름이 버전에 따라 다를 경우 (`opencv_world4100.lib` 등):

```powershell
.\make_msvc.cmd rbf OPENCV_WORLD_LIB=opencv_world4100.lib
```

---

## 5. 빌드

**VS Developer 환경을 자동 설정**해 주는 `make_msvc.cmd` 를 항상 사용:

```powershell
cd C:\...\Camera\get_metadata

# camera_RBF 만 빌드 (가장 자주 사용)
.\make_msvc.cmd rbf

# 전체 빌드 (app, camera_client, camera_RBF)
.\make_msvc.cmd

# 특정 타겟만
.\make_msvc.cmd cam    # camera_client
.\make_msvc.cmd app    # app (메타데이터 덤프 등)
```

빌드 결과물은 모두 `obj-msvc\` 폴더에 생성됩니다:
```
obj-msvc\
  camera_RBF.exe
  camera_RBF.obj
  RTSPClient.obj
  XMLParser.obj
  re_id.obj
```

> **주의**: `make` 를 PowerShell에서 직접 실행하면 `cl` 이 PATH에 없어 실패합니다.
> 반드시 `make_msvc.cmd` 를 사용하세요.

---

## 6. 실행

### 방법 A — `run_rbf.cmd` (권장, 빌드 불필요)

```powershell
.\run_rbf.cmd                          # native 트래커, GStreamer low-latency
.\run_rbf.cmd --no-remote-id           # remote ID 없이
.\run_rbf.cmd --tracker-mode deepsort  # DeepSORT 모드
```

### 방법 B — `make_msvc.cmd` (빌드 후 바로 실행)

```powershell
.\make_msvc.cmd run-rbf-native                         # 빌드 + 실행
.\make_msvc.cmd run-rbf-native ARGS="--no-remote-id"   # 인수 전달
.\make_msvc.cmd run-rbf-deepsort                       # DeepSORT 모드
```

### 실행 시 동작 확인

정상 작동 시 출력:
```
[videoio] available backends: FFMPEG GSTREAMER MSMF DSHOW ...
[camera_RBF] GStreamer pipeline: rtspsrc location=rtsp://... latency=0 ...
[camera_RBF] RTSP open ok: rtsp://...
[camera_RBF] VideoCapture backend=GSTREAMER
[FPS] 29.x
```

---

## 7. GStreamer vs FFmpeg 선택

| | GStreamer | FFmpeg |
|---|---|---|
| 활성화 방법 | `USE_GSTREAMER=1` (run_rbf.cmd 기본값) | 기본값 |
| 딜레이 | `latency=0` + `sync=false` 구성 시 최소 | `fflags nobuffer` + `low_delay` 옵션 |
| H.265/HEVC | 지원 (gst-plugins-bad 설치 시) | 지원 |
| 커스텀 파이프라인 | `GST_PIPELINE=...` 환경변수로 지정 | `OPENCV_FFMPEG_CAPTURE_OPTIONS` |

**GStreamer 파이프라인 커스터마이징** (필요 시 run_rbf.cmd 실행 전 설정):

```powershell
# H.265 카메라
$env:GST_PIPELINE = "rtspsrc location=rtsp://192.168.0.84/profile2/media.smp protocols=tcp latency=0 ! rtph265depay ! h265parse ! avdec_h265 ! videoconvert ! appsink drop=true max-buffers=1 sync=false"

# 하드웨어 디코딩 (NVIDIA)
$env:GST_PIPELINE = "rtspsrc location=rtsp://... latency=0 ! rtph264depay ! h264parse ! nvh264dec ! videoconvert ! appsink drop=true max-buffers=1 sync=false"

.\run_rbf.cmd
```

**FFmpeg으로 되돌리기**:
```powershell
$env:USE_GSTREAMER = ""
.\run_rbf.cmd
```

---

## 8. 문제 해결

### `cl` is not recognized
→ `make_msvc.cmd` 를 사용하지 않고 `make` 를 직접 실행한 경우.
반드시 `.\make_msvc.cmd rbf` 로 실행.

### `LNK1181: cannot open input file 'opencv_world4130.lib'`
→ `OPENCV_ROOT` 경로 또는 `OPENCV_WORLD_LIB` 이름 확인.
```powershell
dir "C:\Users\2-16\Downloads\opencv-gst\install\x64\vc17\lib\*.lib"
```

### `opencv_world4130.dll not found` (exit code 0xC0000135)
→ `run_rbf.cmd` 를 통해 실행하면 자동으로 PATH 설정됨.
직접 실행 시에는 수동으로 PATH 추가:
```powershell
$env:PATH = "C:\Users\2-16\Downloads\opencv-gst\install\x64\vc17\bin;" +
            "C:\Program Files\gstreamer\1.0\msvc_x86_64\bin;" + $env:PATH
```

### GStreamer 백엔드가 뜨지 않음 (`[videoio] available backends: FFMPEG MSMF ...`)
→ GStreamer DLL이 PATH에 없거나 OpenCV가 GStreamer 없이 빌드된 것.
- `gst-launch-1.0.exe --version` 동작 확인
- OpenCV cmake 빌드 시 `GStreamer: YES` 확인

### 영상 딜레이가 큼
→ `USE_GSTREAMER=1` 없이 실행 중이거나 기본 rtspsrc latency(200ms) 적용 중.
→ `run_rbf.cmd` 를 사용하면 자동으로 `latency=0` 파이프라인이 적용됨.
→ 커스텀 파이프라인이 필요하면 `GST_PIPELINE` 환경변수로 지정.

### `[camera_RBF] cap.read failed/buffer empty`
→ 네트워크 불안정 또는 카메라 RTSP 스트림 끊김.
→ `GST_PIPELINE` 에 `protocols=tcp` 포함 여부 확인.
→ `gst-launch-1.0 rtspsrc location=rtsp://카메라IP/... ! fakesink` 로 직접 테스트.
