# SFEPS Client (Qt/QML)

SFEPS Client는 Qt 6/QML 기반 관제 애플리케이션입니다.
주요 기능은 로그인, RTSP 영상·저지연 재생, 이벤트 수신, 음성 통신, 위치/트래킹, 아카이브·분석 화면 연동입니다.

MSVC 환경에서 팀 공용으로 빌드/실행하는 방법은 `MSVC_BUILD_GUIDE.md`를 참고하세요.

## 1. 주요 기능

- 로그인/인증: Auth 서버와 연동된 사용자 로그인
- 실시간 영상 모니터링: RTSP 스트림 수신 및 화면 표시
- 저지연 재생: OpenCV 기반 캡처(선택) 및 GStreamer/FFmpeg 백엔드, ONVIF 메타데이터로 객체 박스 오버레이(환경변수로 켜기/끄기)
- 카메라 제어: Brightness/Contrast CGI 연동
- 이벤트 수신: Fraud Alert 이벤트 실시간 반영
- 위치/트래킹: Position 채널 기반 추적 제어
- 음성 통신: Voice 채널 기반 양방향 오디오
- 비디오 아카이브: 카탈로그 서버 연동 녹화 목록·재생(Archive)
- 분석 화면: AnalyticsView 기반 통계/세션 요약

## 2. 클라이언트 환경

- OS: Windows 10/11
- Qt: 6.10+ (`CMakeLists.txt` 기준; 상세·설치는 `MSVC_BUILD_GUIDE.md`)
- Compiler: MSVC (Visual Studio 2022 x64)
- OpenCV: MSVC 빌드 (예: OpenCV 4.x, vc17). `SFEPS_WITH_OPENCV=OFF`로 비활성화 시 Qt Multimedia 경로만 사용 가능
- GStreamer: RTSP 백엔드를 `gstreamer`로 둘 때 런타임 설치 필요(`run_client.ps1`이 PATH 후보를 잡음)

## 3. 네트워크 구성

기본 포트:
- Auth: 5555 (TLS 6555)
- Fraud Alert: 5557
- Voice: 5556
- Position: 5558
- Video catalog(아카이브): 5559 (`VIDEO_CATALOG_HOST`, `SFEPS_VIDEO_CATALOG_PORT`)
- RTSP: 카메라/환경에 따라 상이(기본 예시는 `run_client.ps1` 참고)

기본 주소는 스크립트·코드 기본값을 따르며, 실제 장비에 맞게 `run_client.ps1`에서 바꿉니다.
- RTSP·메타데이터: `RTSP_STREAM_URL`, 필요 시 `METADATA_RTSP_URL` 분리
- Auth: `AUTH_SERVER_HOST`(AuthManager 기본 호스트와 다를 수 있으므로 스크립트에서 명시 권장)
- Fraud/Position/Video catalog: `FRAUD_SERVER_HOST`, `POS_SERVER_PORT`, `VIDEO_CATALOG_HOST` 등
- 카메라 CGI: 계정·URL은 `CAMERA_*` 환경변수

## 4. 빌드 (Windows MSVC)

```powershell
cd C:\Users\<사용자>\Desktop\SFEPS\client_msvc
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64 ^
  -DOpenCV_DIR="C:/opencv/build/x64/vc17/lib" ^
  -DCMAKE_PREFIX_PATH="C:/Qt/6.10.0/msvc2022_64"
cmake --build build-msvc --config Release
```

참고:
- 소스 트리에 `../Camera/get_metadata`(RTSP/XMLParser·선택 OpenCV RBF)가 있어야 configure/빌드가 됩니다.
- `OpenCV_DIR`를 생략하면 저장소 상위 폴더(`opencv-gst/install` 등)를 자동 탐색합니다.
- OpenCV 없이 빌드: `-DSFEPS_WITH_OPENCV=OFF`
- 실행 파일 이름은 `appHanwhaVisionSFEPS.exe` 입니다.
- 오타 이름(`appHanwhavisionSFEPS.exe`)은 실행되지 않습니다.

## 5. 실행 (권장)

Windows에서는 직접 exe 실행 대신 `run_client.ps1` 사용을 권장합니다.
이 스크립트가 서버/스트림/카메라/TLS 환경변수를 한 번에 설정합니다.

```powershell
cd C:\Users\<사용자>\Desktop\SFEPS\client_msvc
.\run_client.ps1
```

`run_client.cmd`는 `run_client_2.ps1`이 있으면 그쪽을, 없으면 `run_client.ps1`을 호출합니다.

직접 실행(비권장):

```powershell
cd C:\Users\<사용자>\Desktop\SFEPS\client_msvc\build-msvc\Release
.\appHanwhaVisionSFEPS.exe
```

직접 실행 시 `run_client.ps1`에 정의된 환경변수가 빠져 인증/연동 오류가 발생할 수 있습니다.

## 6. 실행 설정 파일 (run_client.ps1)

파일: `client_msvc/run_client.ps1`(동일 내용 복사본이 필요하면 `run_client_2.ps1` 참고)

다음 항목만 환경에 맞게 수정하면 됩니다.

서버/스트림:
- `RTSP_STREAM_URL`
- `METADATA_RTSP_URL`(기본은 영상 URL과 동일), `METADATA_VIDEO_TRACK_ID`, `METADATA_META_TRACK_ID`
- `AUTH_SERVER_HOST`, `FRAUD_SERVER_HOST`, `FRAUD_SERVER_PORT`, `POS_SERVER_PORT`
- 아카이브: `VIDEO_CATALOG_HOST`, `SFEPS_VIDEO_CATALOG_PORT`

저지연/백엔드(요약):
- `SFEPS_DIRECT_STREAM_MODE`(로그인 생략 후 RTSP만)
- `SFEPS_USE_ONVIF_METADATA`, `RTSP_BACKEND`(`ffmpeg` 또는 `gstreamer`), `RTSP_TARGET_FPS`, `RTSP_DROP_GRABS`, `RTSP_FFMPEG_OPTIONS`, `SFEPS_GSTREAMER_PIPELINE`

PWM/추적 연동(라즈베리): `SFEPS_PWM_HOST`, `SFEPS_PWM_PORT` — 배선·수신 측은 `RASPI_PWM_SETUP.md` 참고

카메라 CGI:
- `CAMERA_CGI_USER`
- `CAMERA_CGI_PASSWORD`
- `CAMERA_BRIGHTNESS_CGI_URL` (선택)
- `CAMERA_CONTRAST_CGI_URL` (선택)
- `CAMERA_CGI_ALLOW_INSECURE_TLS`

예시(기본값):
- `CAMERA_BRIGHTNESS_CGI_URL=https://192.168.0.84/stw-cgi/image.cgi?msubmenu=imageenhancements2&action=set&Brightness={value}`
- `CAMERA_CONTRAST_CGI_URL=https://192.168.0.84/stw-cgi/image.cgi?msubmenu=imageenhancements2&action=set&Contrast={value}`

TLS 로그인:
- `AUTH_TLS_ENABLE` (`1`이면 TLS)
- `AUTH_TLS_CA_FILE` (기본: `client_msvc/certs/auth_ca.pem`)
- `AUTH_TLS_PORT`
- `AUTH_PLAINTEXT_PORT`
- `AUTH_ALLOW_PLAINTEXT_FALLBACK`

## 7. 카메라 Brightness/Contrast CGI

현재 클라이언트는 Hanwha CGI를 사용합니다.

- Brightness set:
  - `.../stw-cgi/image.cgi?msubmenu=imageenhancements2&action=set&Brightness={value}`
- Contrast set:
  - `.../stw-cgi/image.cgi?msubmenu=imageenhancements2&action=set&Contrast={value}`
- 현재값 조회(view):
  - `.../stw-cgi/image.cgi?msubmenu=imageenhancements2&action=view`

동작 요약:
- 슬라이더 범위: 1..100
- 앱 시작 시 `action=view`로 현재 Brightness/Contrast 값을 읽어 UI에 반영
- HTTPS 실패 시 HTTP 자동 재시도

## 8. TLS CA 배포 가이드

TLS 로그인 사용 시 서버 CA 인증서를 클라이언트에 배포해야 합니다.

- 복사 대상: `ca.crt` (공개 인증서)
- 금지 대상: `ca.key` (개인키, 절대 클라이언트 배포 금지)
- 배치 경로: `client_msvc/certs/auth_ca.pem`(스크립트가 `AUTH_TLS_CA_FILE`로 설정)

## 9. 빠른 문제 해결

- `401 호스트 인증이 필요함`
  - 카메라 계정/비밀번호 확인
  - `run_client.ps1`로 실행했는지 확인

- `490 Error transferring https://...`
  - 카메라 HTTPS 호환 문제일 수 있음
  - `CAMERA_CGI_ALLOW_INSECURE_TLS=1` 확인
  - 로그에서 HTTP 재시도 성공 여부 확인

- 로그인/알림 서버 연결 실패
  - `AUTH_SERVER_HOST`, `FRAUD_SERVER_HOST`, Auth/Position 포트 설정 확인
  - 서버가 실제로 실행 중인지 확인

- 아카이브 목록/재생 실패
  - `VIDEO_CATALOG_HOST`, `SFEPS_VIDEO_CATALOG_PORT`(기본 5559)와 카탈로그 서버 기동 여부 확인

## 10. 프로젝트 구조

- `src/`: C++ 백엔드 및 QML(`Main.qml`, `VideoDisplay.qml`, `views/*.qml`)
- `assets/`: 이미지/아이콘(빌드 시 `CMake`로 출력 디렉터리에 복사)
- `certs/`: TLS CA 배포 위치(`auth_ca.pem`)
- `resources.qrc`: Qt 리소스
- `HanwhaVisionSFEPS.qmlproject`: Qt Creator/QML 도구용 프로젝트 파일
- `run_client.ps1` / `run_client.cmd`: Windows 실행(권장)
- `MSVC_BUILD_GUIDE.md`: 팀 공용 MSVC 설치·빌드 상세
- `RASPI_PWM_SETUP.md`: PWM 출력 연동(하드웨어 측)
- `CMakeLists.txt`: 빌드 설정
