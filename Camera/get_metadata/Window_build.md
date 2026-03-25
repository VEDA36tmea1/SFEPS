# Windows Build/Run Guide (camera_RBF)

## 1) Prerequisites

- Visual Studio 2022 (MSVC toolchain)
- GStreamer MSVC x86_64 installed:
  - `C:\Program Files\gstreamer\1.0\msvc_x86_64`
- OpenCV rebuilt with GStreamer enabled (`GStreamer: YES`)
  - install root example: `C:/Users/2-16/Downloads/opencv-gst/install`

## 2) OpenCV (with GStreamer) configure/build

Run in **Developer PowerShell for VS 2022**:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64
cd C:\Users\2-16\Downloads\opencv
mkdir build_gst -ErrorAction SilentlyContinue
cd build_gst

cmake -G "Visual Studio 17 2022" -A x64 `
  -D CMAKE_BUILD_TYPE=Release `
  -D CMAKE_INSTALL_PREFIX="C:/Users/2-16/Downloads/opencv-gst/install" `
  -D BUILD_LIST=core,highgui,imgproc,videoio,imgcodecs `
  -D BUILD_opencv_world=ON `
  -D WITH_GSTREAMER=ON `
  -D WITH_FFMPEG=ON `
  -D CMAKE_PREFIX_PATH="C:/Program Files/gstreamer/1.0/msvc_x86_64" `
  -D OPENCV_GENERATE_PKGCONFIG=ON `
  ../sources

cmake --build . --config Release --target INSTALL -j 8
```

Check output contains:

- `Video I/O: GStreamer: YES`

## 3) Build camera_RBF (this repo)

`Makefile` already targets:

- `OPENCV_ROOT=C:/Users/2-16/Downloads/opencv-gst/install`
- `x64/vc17/lib`, `x64/vc17/bin`
- GStreamer runtime/plugin path

Build command:

```powershell
cd C:\Users\2-16\Desktop\SFEPS\Camera\get_metadata
make -B camera_RBF.exe
```

## 4) Run commands

Default run:

```powershell
make run-rbf
```

Run with arguments (`ARGS` passthrough):

```powershell
make run-rbf ARGS="--tracker-mode native"
make run-rbf ARGS="--tracker-mode deepsort --predict-ms 200"
make run-rbf ARGS="--tracker-mode native --ratio 0.35 --send-interval-ms 2000"
```

## 5) Tracker mode options

- `--tracker-mode deepsort`
  - Existing Python DeepSORT worker path (default)
- `--tracker-mode native`
  - New C++ native tracker path (no Python IPC)

## 6) Troubleshooting

- `cl is not recognized`
  - Run in VS Developer PowerShell first.
- `LNK1181 opencv_world4130.lib`
  - Verify `OPENCV_ROOT` and `x64/vc17/lib` path.
- Exit code `-1073741515 (0xC0000135)`
  - Missing runtime DLL in PATH.
  - `run-rbf` already sets OpenCV/GStreamer PATH for current command.
- GStreamer open fails and fallback occurs
  - Confirm OpenCV build info says `GStreamer: YES`.
  - Confirm `gst-launch-1.0 --version` works.
