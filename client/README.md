# SFEPS - 지하철 부정 승차 방지 시스템 (Subway Fare Evasion Prevention System)

**SFEPS**는 **Qt 6 / QML 클라이언트**와 **라즈베리파이 (MariaDB + GStreamer) 서버**를 결합하여 실시간으로 부정 승차 및 이상 징후를 감지하고 관리하는 지능형 통합 보안 시스템입니다.

---

## 🚀 주요 기능

*   **보안 로그인 시스템**: 라즈베리파이 내 MariaDB와 연동된 TCP/IP 소켓 통신(Port 5555)을 통한 사용자 인증.
*   **멀티 프로토콜 스트리밍**: OpenCV를 활용한 **RTSP 실시간 영상** 수신 및 고속 재생 지원.
    *   **저지연 최적화 (Low Latency)**: `VideoCaptureWorker` 스레드와 `CAP_PROP_BUFFERSIZE` 최적화를 통해 실시간 응답성 확보.
*   **실시간 영상 처리**: OpenCV 기반의 밝기 조절 및 ROI(관심 영역) 드래그 줌 기능 구현.
*   **이상 징후 실시간 알림**: `FraudManager`를 통해 서버로부터 부정 승차 의심 데이터를 즉각 수신(Port 5557) 및 팝업 알림.
*   **서버 단절 강제 로그아웃 UX**: 서버 연결 단절 또는 `AUTH|FORCE_LOGOUT` 이벤트 수신 시 안내 팝업 표시 후 자동 로그아웃 처리.
*   **Position 복구 강화**: 재로그인 시 `TEST|LOGIN_OK` ACK 기반으로 Position 채널(Port 5558) 연결을 재시도하고, ACK 지연 시 폴백 타이머로 자동 복구.
*   **양방향 음성 통신**: `VoiceManager`를 통한 RAW PCM 무전통신 기능(Port 5556, 16kHz Mono) 지원.
*   **통합 관제 대시보드**: 모니터링, 데이터 분석(Analytics), 로그 관리, 설정 기능을 갖춘 유려한 다크 테마 UI.

---

## 🛠️ 시스템 요구 사항

### 클라이언트 환경 (Client)
*   **OS**: Windows 10/11
*   **Framework**: Qt 6.4 이상 (필수: `QtQuick`, `QtMultimedia`, `QtNetwork`)
*   **Compiler**: MinGW 64-bit (권장)
*   **Library**: OpenCV 4.5.5 (MinGW 빌드)
*   **Network Ports**:
    *   TCP Port `5555`: 사용자 인증 (Auth)
    *   TCP Port `5557`: 부정 승차 알림 수신 (Fraud Alert)
    *   TCP Port `5556`: 음성 스트리밍 (Voice/Audio)
    *   TCP Port `5558`: 위치/트래킹 스트림 (Position)
    *   RTSP Port `8554`: 영상 스트리밍 (Video)

### 네트워크 설정 (필수)
현재 코드는 라즈베리파이 서버 IP를 `192.168.0.82`로 가정하고 있습니다. 서버 환경에 맞춰 다음 파일들을 확인하십시오.
*   `src/authmanager.cpp`: 인증 서버 IP/Port 설정.
*   `src/mainwindow.cpp`: RTSP 주소 (`rtsp://192.168.0.82:8554/cam1`) 설정.
*   `src/voicemanager.cpp`: 오디오 서버 IP/Port 설정.

환경변수로 런타임 네트워크 대상을 변경할 수도 있습니다.
*   `AUTH_SERVER_HOST`: 로그인 인증 서버 호스트(기본값 `192.168.0.82`)
*   `AUTH_TLS_ENABLE`: 로그인 채널 TLS 사용 여부(기본값 `1`)
*   `AUTH_TLS_PORT`: 로그인 TLS 포트(기본값 `6555`)
*   `AUTH_PLAINTEXT_PORT`: 로그인 평문 포트(기본값 `5555`)
*   `AUTH_ALLOW_PLAINTEXT_FALLBACK`: TLS 실패 시 평문 1회 재시도 허용(기본값 `0`)
*   `AUTH_TLS_CA_FILE`: 서버 인증서 검증용 CA PEM 파일 경로(예: `.../client/certs/auth_ca.pem`)
*   `RTSP_STREAM_URL`: 모니터링 RTSP 스트림 URL(기본값 `rtsp://192.168.0.82:8554/cam1`)
*   `FRAUD_SERVER_HOST`: 알림 서버 호스트(기본값 `192.168.0.82`)
*   `FRAUD_SERVER_PORT`: 알림 서버 평문 포트(기본값 `5557`)
*   `POS_SERVER_HOST`: Position 서버 호스트(기본값: `FRAUD_SERVER_HOST` 값)
*   `POS_SERVER_PORT`: Position 서버 평문 포트(기본값 `5558`)
*   `SFEPS_ALERT_TLS_ENABLE`, `SFEPS_ALERT_TLS_PORT`: 알림 채널 TLS 사용/포트
*   `SFEPS_POS_TLS_ENABLE`, `SFEPS_POS_TLS_PORT`: Position 채널 TLS 사용/포트

스트리밍 장애가 발생하면 Monitoring 화면에서 `CONNECTING/RECONNECTING/STREAM OFFLINE` 상태가 표시되며,
클라이언트는 3초 간격으로 자동 재연결을 시도합니다.

서버 연결이 장시간 복구되지 않거나 서버가 강제 로그아웃 이벤트를 전송하면,
클라이언트는 강제 로그아웃 안내 팝업을 띄운 후 로그인 화면으로 전환합니다.

### TLS 관련 클라이언트 환경변수 (새)

- `SFEPS_CLIENT_TLS_ENABLE`: `0` 또는 `1`. `1`이면 클라이언트는 auth/alert/audio 채널에서 TLS(`QSslSocket`)로 접속을 시도합니다. 제공되지 않으면 기존 `AUTH_TLS_ENABLE`을 참고합니다.
- `SFEPS_CLIENT_CA_FILE`: TLS 모드에서 사용할 CA PEM 파일 경로. (예: `C:/path/to/ca.pem`) 클라이언트는 해당 CA로 서버 인증서를 검증합니다.
- `SFEPS_CLIENT_TLS_SERVER_NAME`: TLS 호스트명 검증용 서버 이름(SAN 또는 CN과 일치해야 함). 비어있으면 연결 대상의 호스트명을 사용합니다.

포트 매핑(기본)
- Plain: Auth `5555`, Audio `5556`, Alert `5557`
- TLS: Auth `6555`, Audio `6556`, Alert `6557`

동작 요약
- TLS 모드(`SFEPS_CLIENT_TLS_ENABLE=1`)일 때는 `QSslSocket`을 사용하고 `connectToHostEncrypted()`를 호출합니다.
- 서버 인증서 검증은 필수이며(`VerifyPeer`), `ignoreSslErrors()`는 사용하지 않습니다.
- TLS 검증 실패(잘못된 CA, 호스트명 불일치 등)가 발생하면 TLS 연결은 실패로 처리됩니다. 인증 채널의 경우 평문 폴백 동작은 `AUTH_ALLOW_PLAINTEXT_FALLBACK` 환경변수로 제어됩니다(기본: 비허용).


---

## ⚙️ 설정 및 빌드 방법

### 1. 사전 준비 (OpenCV)
이 프로젝트는 **OpenCV MinGW 빌드 (`OpenCV-4.5.5-x64`)**가 필요합니다.
프로젝트 루트의 **상위 디렉토리**에 다운로드하거나 `CMakeLists.txt`에서 `OpenCV_DIR` 경로를 수정해야 합니다.

```powershell
# 예시: 이 프로젝트의 상위 폴더에서 실행
git clone --branch OpenCV-4.5.5-x64 --depth 1 https://github.com/huihut/OpenCV-MinGW-Build.git OpenCV-MinGW-Build
```

### 2. 빌드 방법 (Command Line)
1.  **PowerShell** 또는 터미널을 엽니다.
2.  프로젝트 디렉토리로 이동합니다:
    ```powershell
    cd C:\Users\2-08\Desktop\SFEPS\client
    ```
3.  **빌드 디렉토리 생성**:
    ```powershell
    mkdir build-mingw
    cd build-mingw
    ```
4.  **CMake 구성 및 빌드**

    - Qt/CMake 경로와 OpenCV 경로를 명시해 주세요. 예:
    ```powershell
    cmake -G "MinGW Makefiles" \
      -DOpenCV_DIR="C:/Users/2-08/OpenCV-MinGW-Build/x64/mingw" \
      -DCMAKE_PREFIX_PATH="C:/Qt/6.10.0/mingw_64/lib/cmake" \
      ..
    cmake --build . --config Release
    # 또는 병렬 빌드
    mingw32-make -j4
    ```

    - 만약 CMake가 Qt를 못 찾는다면 `-DCMAKE_PREFIX_PATH`에 Qt의 `lib/cmake` 경로를 지정하세요.
    - OpenCV의 `OpenCVConfig.cmake`가 있는 디렉토리를 `-DOpenCV_DIR`로 지정해야 합니다.

### 3. 애플리케이션 실행
빌드가 성공하면 빌드 디렉토리 내의 실행 파일을 실행합니다:
```powershell
.\appHanwhaVisionSFEPS.exe
    ```

또는 완전한 예:
```powershell
cd C:\Users\2-08\Desktop\SFEPS\client\build-mingw
.\appHanwhaVisionSFEPS.exe
```

### 4. TLS 로그인용 CA 설정 (중요)
서버의 CA **인증서**(`ca.crt`)를 클라이언트 `certs/auth_ca.pem`으로 배포해야 TLS 검증이 성공합니다.

*   복사 대상: `ca.crt` (공개 인증서)
*   금지 대상: `ca.key` (개인키, 절대 클라이언트 배포 금지)

Linux/WSL에서 실행할 때는 아래 스크립트로 환경변수를 자동 설정할 수 있습니다:

```bash
cd client
./run_client_tls.sh ./build/appHanwhaVisionSFEPS
```

---

## 빌드 중 발견된 컴파일 문제

- 빌드 중 `QSslConfiguration` 관련 incomplete type 에러가 발생할 수 있습니다. 이 경우 소스 파일 `src/authmanager.cpp`에
    `#include <QSslConfiguration>` 헤더가 누락되어 있을 수 있으므로 추가하면 해결됩니다. (현재 저장소에 해당 패치가 적용되어 있습니다.)

## 윈도우 배포 팁

- 런타임에 DLL 누락 에러가 발생하면 Qt와 OpenCV의 `bin` 폴더를 `PATH`에 추가하거나 필요한 DLL들을 실행파일 옆에 복사하세요.
- Qt 배포 도구 사용 예:
```powershell
# Qt의 windeployqt로 필요한 Qt DLL과 QML 종속성을 복사
C:\Qt\6.10.0\mingw_64\bin\windeployqt.exe --qmldir ..\src appHanwhaVisionSFEPS.exe
```

## 📂 프로젝트 구조

*   `src/`: C++/QML 소스 코드.
    *   `main.cpp`: 프로그램 진입점 및 QML 타입 등록.
    *   `mainwindow.h/cpp`: OpenCV 기반 영상 스트리밍 및 UI 핵심 로직 (`QQuickPaintedItem`).
    *   `authmanager.h/cpp`: TCP 소켓 기반 로그인/인증 처리.
    *   `fraudmanager.h/cpp`: 서버로부터 실시간 부정 승차 알림 수신용 소켓 관리.
    *   `voicemanager.h/cpp`: 마이크 입력 캡처 및 서버 전송 (Voice over IP).
    *   `views/`: 기능별 QML 화면 (Monitoring, Analytics, Login, Settings 등).
*   `assets/`: 이미지, 아이콘 및 스타일 리소스.
*   `CMakeLists.txt`: 프로젝트 빌드 설정.
