# SFEPS - 지하철 부정 승차 방지 시스템 (Subway Fare Evasion Prevention System)

**SFEPS**는 **Qt 6 / QML 클라이언트**와 **라즈베리파이 (MariaDB + GStreamer) 서버**를 결합하여 실시간으로 부정 승차 및 이상 징후를 감지하고 관리하는 지능형 통합 보안 시스템입니다.

---

## 🚀 주요 기능

*   **보안 로그인 시스템**: 라즈베리파이 내 MariaDB와 연동된 TCP/IP 소켓 통신(Port 5555)을 통한 사용자 인증.
*   **멀티 프로토콜 스트리밍**: OpenCV를 활용한 **RTSP 실시간 영상** 수신 및 고속 재생 지원.
    *   **저지연 최적화 (Low Latency)**: `VideoCaptureWorker` 스레드와 `CAP_PROP_BUFFERSIZE` 최적화를 통해 실시간 응답성 확보.
*   **실시간 영상 처리**: OpenCV 기반의 밝기 조절 및 ROI(관심 영역) 드래그 줌 기능 구현.
*   **이상 징후 실시간 알림**: `FraudManager`를 통해 서버로부터 부정 승차 의심 데이터를 즉각 수신(Port 5557) 및 팝업 알림.
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
    *   RTSP Port `8554`: 영상 스트리밍 (Video)

### 네트워크 설정 (필수)
현재 코드는 라즈베리파이 서버 IP를 `192.168.0.92`로 가정하고 있습니다. 서버 환경에 맞춰 다음 파일들을 확인하십시오.
*   `src/authmanager.cpp`: 인증 서버 IP/Port 설정.
*   `src/mainwindow.cpp`: RTSP 주소 (`rtsp://192.168.0.92:8554/cam1`) 설정.
*   `src/voicemanager.cpp`: 오디오 서버 IP/Port 설정.

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
    cd C:\path\to\SFEPS\client
    ```
3.  **빌드 디렉토리 생성**:
    ```powershell
    mkdir build
    cd build
    ```
4.  **CMake 구성 및 빌드**:
    ```powershell
    cmake -G "MinGW Makefiles" ..
    cmake --build .
    ```

### 3. 애플리케이션 실행
빌드가 성공하면 빌드 디렉토리 내의 실행 파일을 실행합니다:
```powershell
.\appHanwhaVisionSFEPS.exe
    ```

---

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
