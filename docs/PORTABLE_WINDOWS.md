# Portable Windows Build/Run Guide

이 문서는 `SFEPS`를 다른 Windows PC에서도 동일하게 빌드/실행하기 위한 고정 템플릿 가이드입니다.
목표는 경로 하드코딩을 줄이고, `thirdparty` 기준으로 의존성을 통일하는 것입니다.

## 1) 목표 환경

- OS: Windows 10/11 x64
- IDE/Toolchain:
  - Visual Studio 2022 (Desktop development with C++)
  - CMake 3.16 이상 (권장: 3.24+)
- Qt:
  - Qt 6.10.0 (`msvc2022_64`) 권장
  - 필수 모듈: `Quick`, `QuickControls2`, `Network`, `Multimedia`

## 2) thirdparty 준비물 (버전/구성)

권장 저장소 구조:

```text
<repoRoot>\
  thirdparty\
    opencv-gst\
      install\
        OpenCVConfig.cmake
        x64\vc17\bin\opencv_*.dll
        x64\vc17\lib\opencv_*.lib
```

### OpenCV 빌드 옵션 명시 (필수)

OpenCV는 아래 옵션으로 준비된 바이너리를 사용합니다.

- `WITH_GSTREAMER=ON`
- `WITH_FFMPEG=ON`
- `OPENCV_INSTALL_PREFIX=<repoRoot>/thirdparty/opencv-gst/install`

확인 포인트:

- `OpenCVConfig.cmake` 존재
- 런타임 DLL 경로 존재: `x64\vc17\bin`
- 라이브러리 경로 존재: `x64\vc17\lib`

### MSVC ABI 명시 (필수)

- 본 프로젝트는 MSVC 2022 기준 ABI를 사용합니다.
- OpenCV ABI도 동일해야 하며, 경로가 `x64\vc17\...` 형태인지 확인하세요.
- ABI 불일치(예: MinGW 빌드 OpenCV 혼용) 시 링크/실행 오류가 발생합니다.

## 3) 빌드 방법 (OpenCV_DIR = thirdparty 기준)

PowerShell 예시:

```powershell
cd <repoRoot>
cmake -S client_msvc -B build-opencv-on -G "Visual Studio 17 2022" -A x64 `
  -DSFEPS_WITH_OPENCV=ON `
  -DOpenCV_DIR="<repoRoot>\thirdparty\opencv-gst\install\OpenCVConfig.cmake" `
  -DCMAKE_PREFIX_PATH="<Qt설치경로>\msvc2022_64"

cmake --build build-opencv-on --config Release
```

빌드 결과:

- `build-opencv-on\Release\appHanwhaVisionSFEPS.exe`

## 4) 실행 방법

권장 실행:

```powershell
cd <repoRoot>\client_msvc
.\run_client.ps1
```

`run_client.ps1`에서 사용하는 주요 환경변수(대표):

- 스트림/메타:
  - `RTSP_STREAM_URL`
  - `METADATA_RTSP_URL`
  - `METADATA_VIDEO_TRACK_ID`
  - `METADATA_META_TRACK_ID`
- 저지연 옵션:
  - `RTSP_BACKEND` (`gstreamer` 또는 `ffmpeg`)
  - `SFEPS_GSTREAMER_PIPELINE`
  - `RTSP_TARGET_FPS`
  - `RTSP_DROP_GRABS`
  - `RTSP_FFMPEG_OPTIONS`
- 동작 모드:
  - `SFEPS_DIRECT_STREAM_MODE`
  - `SFEPS_USE_ONVIF_METADATA`
- 서버 연결:
  - `FRAUD_SERVER_HOST`
  - `FRAUD_SERVER_PORT`
  - `POS_SERVER_PORT`
- TLS/인증:
  - `AUTH_TLS_ENABLE`
  - `AUTH_TLS_CA_FILE`
  - `AUTH_TLS_PORT`
  - `AUTH_PLAINTEXT_PORT`
- 카메라 CGI:
  - `CAMERA_CGI_USER`
  - `CAMERA_CGI_PASSWORD`
  - `CAMERA_CGI_ALLOW_INSECURE_TLS`

### PATH / GST_PLUGIN_PATH 규칙 (상대경로 기반 권장)

문서 표준 규칙:

- OpenCV DLL: `<repoRoot>\thirdparty\opencv-gst\install\x64\vc17\bin`
- GStreamer bin: `<gstreamer_root>\bin` (또는 팀 포터블 경로)
- GStreamer plugin: `<gstreamer_root>\lib\gstreamer-1.0`
- Qt bin: `<Qt설치경로>\msvc2022_64\bin`

실행 전 스크립트는 위 경로를 `PATH` 및 `GST_PLUGIN_PATH`에 반영해야 합니다.

## 5) 체크리스트 (런타임 DLL/플러그인 문제 대응)

- [ ] `opencv_*.dll`이 `PATH`에서 검색되는가?
  - 예: `thirdparty\opencv-gst\install\x64\vc17\bin`
- [ ] `GST_PLUGIN_PATH`가 올바른 플러그인 폴더를 가리키는가?
  - 예: `...\gstreamer\1.0\msvc_x86_64\lib\gstreamer-1.0`
- [ ] 실행 로그에서 OpenCV + gstreamer backend 사용이 확인되는가?
  - 필요 시 `RTSP_BACKEND=gstreamer`로 고정하여 검증
- [ ] `OpenCV_DIR`가 `OpenCVConfig.cmake` 파일 경로까지 정확히 지정되었는가?
- [ ] Qt 런타임 경로(`msvc2022_64\bin`)가 `PATH`에 반영되었는가?

## 6) 자주 발생하는 문제

- `Could not find OpenCVConfig.cmake`
  - `-DOpenCV_DIR` 경로 오타 또는 파일 경로 미지정
- 실행 시 `0xC0000135` 또는 DLL 누락
  - `PATH`에 OpenCV/Qt/GStreamer 런타임 경로 추가 필요
- GStreamer backend 오픈 실패
  - OpenCV가 `WITH_GSTREAMER=ON`으로 빌드되었는지 확인
  - `GST_PLUGIN_PATH`가 유효한지 확인

## 7) 팀 규칙 (권장)

- 개인 PC 절대경로(예: `C:\Users\<name>\...`)를 코드/스크립트에 직접 박지 않는다.
- `thirdparty` 기준 상대경로 또는 환경변수 기반으로 의존성 경로를 해석한다.
- 문서와 스크립트의 경로 규칙을 항상 동일하게 유지한다.
