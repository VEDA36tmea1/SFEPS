# SFEPS Client (Qt/QML)

SFEPS Client는 Qt 6/QML 기반 관제 애플리케이션입니다.
주요 기능은 로그인, RTSP 영상 모니터링, 이벤트 수신, 음성 통신, 위치/트래킹 연동입니다.

## 1. 주요 기능

- 로그인/인증: Auth 서버와 연동된 사용자 로그인
- 실시간 영상 모니터링: RTSP 스트림 수신 및 화면 표시
- 카메라 제어: Brightness/Contrast CGI 연동
- 이벤트 수신: Fraud Alert 이벤트 실시간 반영
- 위치/트래킹: Position 채널 기반 추적 제어
- 음성 통신: Voice 채널 기반 양방향 오디오

## 2. 클라이언트 환경

- OS: Windows 10/11
- Qt: 6.4+
- Compiler: MinGW 64-bit
- OpenCV: MinGW 빌드(예: OpenCV 4.5.5)

## 3. 네트워크 구성

기본 포트:
- Auth: 5555 (TLS 6555)
- Fraud Alert: 5557 (TLS 6557)
- Voice: 5556 (TLS 6556)
- Position: 5558 (TLS 6558)
- Video Catalog: 5559 (TLS 6559)
- RTSP: 8554

기본 주소(기본값):
- RTSP 스트리밍: `rtsp://192.168.0.101:8554/cam1`
- Auth/Fraud/Position 서버: `192.168.0.101`
- 카메라 CGI 서버: `192.168.0.84`

주요 대상 주소는 `run_client.ps1`에서 변경합니다.

## 4. 빌드 (Windows MinGW)

```powershell
cd C:\Users\2-08\Desktop\SFEPS\client
mkdir build-mingw
cd build-mingw

cmake -G "MinGW Makefiles" ^
  -DOpenCV_DIR="C:/Users/2-08/OpenCV-MinGW-Build/x64/mingw" ^
  -DCMAKE_PREFIX_PATH="C:/Qt/6.10.0/mingw_64/lib/cmake" ^
  ..

cmake --build . --config Release
```

참고:
- 실행 파일 이름은 `appHanwhaVisionSFEPS.exe` 입니다.
- 오타 이름(`appHanwhavisionSFEPS.exe`)은 실행되지 않습니다.

## 5. 실행 (권장)

Windows에서는 직접 exe 실행 대신 `run_client.ps1` 사용을 권장합니다.
이 스크립트가 서버/스트림/카메라/TLS 환경변수를 한 번에 설정합니다.

```powershell
cd C:\Users\2-08\Desktop\SFEPS\client
.\run_client.ps1
```

직접 실행(비권장):

```powershell
cd C:\Users\2-08\Desktop\SFEPS\client\build-mingw
.\appHanwhaVisionSFEPS.exe
```

직접 실행 시 `run_client.ps1`에 정의된 환경변수가 빠져 인증/연동 오류가 발생할 수 있습니다.

## 6. 실행 설정 파일 (run_client.ps1)

파일: `client/run_client.ps1`

다음 항목만 환경에 맞게 수정하면 됩니다.

서버/스트림:
- `RTSP_STREAM_URL`
- `FRAUD_SERVER_HOST`, `FRAUD_SERVER_PORT`
- `POS_SERVER_PORT`

예시(기본값):
- `RTSP_STREAM_URL=rtsp://192.168.0.101:8554/cam1`
- `FRAUD_SERVER_HOST=192.168.0.101`
- `FRAUD_SERVER_PORT=5557`
- `POS_SERVER_PORT=5558`

카메라 CGI:
- `CAMERA_CGI_USER`
- `CAMERA_CGI_PASSWORD`
- `CAMERA_BRIGHTNESS_CGI_URL` (선택)
- `CAMERA_CONTRAST_CGI_URL` (선택)
- `CAMERA_CGI_ALLOW_INSECURE_TLS`

예시(기본값):
- `CAMERA_BRIGHTNESS_CGI_URL=https://192.168.0.84/stw-cgi/image.cgi?msubmenu=imageenhancements2&action=set&Brightness={value}`
- `CAMERA_CONTRAST_CGI_URL=https://192.168.0.84/stw-cgi/image.cgi?msubmenu=imageenhancements2&action=set&Contrast={value}`

TLS:
- `AUTH_TLS_ENABLE` (`1`이면 Auth TLS)
- `AUTH_TLS_CA_FILE` (기본: `client/certs/auth_ca.pem`)
- `SFEPS_CLIENT_TLS_ENABLE` (Alert/Voice/Position 공통 TLS 기본 토글)
- `SFEPS_ALERT_TLS_ENABLE`, `SFEPS_ALERT_TLS_PORT`
- `SFEPS_POS_TLS_ENABLE`, `SFEPS_POS_TLS_PORT`
- `SFEPS_AUDIO_TLS_PORT`
- `SFEPS_VIDEO_CATALOG_TLS_ENABLE`, `SFEPS_VIDEO_CATALOG_TLS_PORT`
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

## 8. 영상 보관함 (Video Archive)

Analytics > Video Storage에서 서버 보관 영상 목록 조회 및 재생.

**주요 컴포넌트**:
- `VideoArchiveManager` (C++): Video Catalog 서버 TCP/SSL 연결, Push 프로토콜 파싱
- `RecordingListModel` (C++): QAbstractListModel 구현, 실시간 add/delete 처리
- `ArchiveView.qml`: 영상 목록 전시 (DATE/TIME/ACTION 칼럼), 모달 팝업 재생

**환경변수** (`run_client.ps1`):
- `VIDEO_CATALOG_HOST`: Video Catalog 서버 주소 (기본값: 127.0.0.1)
- `SFEPS_VIDEO_CATALOG_PORT`: TCP 포트 (기본값: 5559)
- `SFEPS_VIDEO_CATALOG_TLS_ENABLE`: TLS 사용 여부 (기본값: 0)
- `SFEPS_VIDEO_CATALOG_TLS_PORT`: TLS 포트 (기본값: 6559)

**프로토콜**:
- 스냅샷: `REC_SNAPSHOT_BEGIN|TOTAL=N` → `REC|id|createdAt` 반복 → `REC_SNAPSHOT_END|TOTAL=N`
- 실시간: `REC_ADD|id|createdAt`, `REC_DEL|id`
- 재생: `PLAY_RECORD|id` 요청 → `PLAY_URL|id|createdAt|url` 응답

## 9. TLS CA 배포 가이드

TLS 로그인 사용 시 서버 CA 인증서를 클라이언트에 배포해야 합니다.

- 복사 대상: `ca.crt` (공개 인증서)
- 금지 대상: `ca.key` (개인키, 절대 클라이언트 배포 금지)
- 배치 경로: `client/certs/auth_ca.pem`

## 10. 빠른 문제 해결

- `401 호스트 인증이 필요함`
  - 카메라 계정/비밀번호 확인
  - `run_client.ps1`로 실행했는지 확인

- `490 Error transferring https://...`
  - 카메라 HTTPS 호환 문제일 수 있음
  - `CAMERA_CGI_ALLOW_INSECURE_TLS=1` 확인
  - 로그에서 HTTP 재시도 성공 여부 확인

- 로그인/알림 서버 연결 실패
  - `FRAUD_SERVER_HOST`, Auth 포트/Position 포트 설정 확인
  - 서버가 실제로 실행 중인지 확인

## 11. 프로젝트 구조

- `src/`: C++/QML 소스
- `assets/`: 이미지/아이콘 리소스
- `run_client.ps1`: Windows 실행 스크립트 (권장)
- `CMakeLists.txt`: 빌드 설정
