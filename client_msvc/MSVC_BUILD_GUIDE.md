# SFEPS Client MSVC 빌드 가이드

이 문서는 `client_msvc`를 Windows + MSVC 환경에서 팀원들이 동일하게 빌드/실행하기 위한 안내서입니다.

## 1) 필수 설치 항목

- Windows 10/11 x64
- Visual Studio 2022 Community 이상
  - 워크로드: `Desktop development with C++`
- CMake (3.16+ 권장)
- Qt 6.10.0 (또는 6.4+)
  - 툴체인: `msvc2022_64`
- OpenCV Windows prebuilt (MSVC용)
  - 예시 버전: 4.x

## 2) 권장 폴더 구조

아래는 예시이며, 각자 경로를 다르게 써도 됩니다.

- 저장소: `C:\Users\<사용자>\Desktop\SFEPS`
- OpenCV: `C:\Users\<사용자>\Downloads\opencv-gst\install`
- GStreamer: `C:\Program Files\gstreamer\1.0\msvc_x86_64`
- Qt: `C:\Qt\6.10.0\msvc2022_64`

중요:
- OpenCV를 반드시 저장소 안으로 옮길 필요는 없습니다.
- 대신 CMake 실행 시 `OpenCV_DIR`를 정확히 전달하면 됩니다.

## 3) OpenCV 다운로드/설치

1. OpenCV 공식 릴리스에서 Windows 패키지 다운로드
2. 압축 해제 후 `build` 폴더 위치 확인
3. 아래 파일이 존재하는지 확인:
   - `OpenCVConfig.cmake`
   - `x64\vc17\lib\opencv_*.lib`
   - `x64\vc17\bin\opencv_*.dll`

예시 경로:
- `C:\Users\<사용자>\Downloads\opencv-gst\install\OpenCVConfig.cmake`

## 4) 빌드 방법 (MSVC)

PowerShell에서:

```powershell
cd C:\Users\<사용자>\Desktop\SFEPS\client_msvc

cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64 `
  -DOpenCV_DIR="C:/Users/<사용자>/Downloads/opencv-gst/install" `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.10.0/msvc2022_64"

cmake --build build-msvc --config Release
```

빌드 결과:
- `client_msvc\build-msvc\Release\appHanwhaVisionSFEPS.exe`

## 5) 실행 방법

권장:

```powershell
cd C:\Users\<사용자>\Desktop\SFEPS\client_msvc
.\run_client.ps1
```

실행 정책 입력을 매번 하기 싫다면:

```powershell
cd C:\Users\<사용자>\Desktop\SFEPS\client_msvc
.\run_client.cmd
```

`run_client.cmd`는 내부에서 `powershell -ExecutionPolicy Bypass -File .\run_client.ps1`로 실행한다.

`run_client.ps1`가 아래를 자동 처리합니다.
- 서버/RTSP 환경변수 세팅
- Qt/OpenCV DLL 경로(PATH) 추가

### 저지연 직접 RTSP 모드 (로그인 없이 영상 확인)

`client_msvc/run_client.ps1`에서 아래 값을 설정:

```powershell
$env:SFEPS_DIRECT_STREAM_MODE = "1"
$env:RTSP_BACKEND = "gstreamer"   # 필요 시 "ffmpeg"로 변경
$env:RTSP_TARGET_FPS = "30"
$env:RTSP_DROP_GRABS = "3"
```

설명:
- `SFEPS_DIRECT_STREAM_MODE=1`: 로그인/Auth/Fraud/Position 연결을 건너뛰고 메인 화면에서 RTSP 직접 재생
- `SFEPS_USE_ONVIF_METADATA=1`: ONVIF 메타데이터를 직접 수신해 `XMLParser` 기반 Human bbox 오버레이
- `RTSP_BACKEND`: OpenCV 백엔드 선택 (기본 `gstreamer`, 문제 시 `ffmpeg`로 변경)
- `RTSP_TARGET_FPS`: UI에 표시할 목표 FPS
- `RTSP_DROP_GRABS`: 실시간성 유지 위해 grab 프레임을 추가로 버리는 개수 (클수록 지연 감소, 프레임 손실 증가)

메타데이터 기준 통일:
- 직접 파싱한 각 bbox에 `metaFrameNo`, `metaTimestamp`, `metaTsMs`를 함께 태깅한다.
- `metaFrameNo`: 메타데이터 프레임 단위 증가 카운터
- `metaTimestamp`: RTSP/RTP timestamp
- `metaTsMs`: 클라이언트 수신 시각(ms)

직접 실행(비권장):

```powershell
cd C:\Users\<사용자>\Desktop\SFEPS\client_msvc\build-msvc\Release
.\appHanwhaVisionSFEPS.exe
```

## 6) 팀원 체크리스트

- [ ] Visual Studio 2022 C++ 워크로드 설치
- [ ] Qt `msvc2022_64` 설치
- [ ] OpenCV `build` 폴더 준비
- [ ] `cmake -DOpenCV_DIR=...` 경로를 본인 PC 경로로 입력
- [ ] `cmake --build build-msvc --config Release` 성공
- [ ] `run_client.ps1`로 실행 확인

## 7) 자주 나는 에러와 해결

- `Could not find OpenCVConfig.cmake`
  - `-DOpenCV_DIR`를 `.../opencv-gst/install`로 지정했는지 확인
- 실행 시 DLL 누락 에러
  - `run_client.ps1`로 실행
  - 또는 PATH에 아래 추가:
    - `...\opencv-gst\install\x64\vc17\bin`
    - `C:\Program Files\gstreamer\1.0\msvc_x86_64\bin`
    - `C:\Qt\6.10.0\msvc2022_64\bin`
- Qt 패키지 탐색 실패
  - `-DCMAKE_PREFIX_PATH="C:/Qt/6.10.0/msvc2022_64"` 확인

