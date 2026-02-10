# SFEPS - 지하철 부정 승차 방지 시스템 (Subway Fare Evasion Prevention System)

**SFEPS**는 **Qt 6 / QML 클라이언트**와 **라즈베리파이 (MariaDB + GStreamer) 서버**를 결합하여 실시간으로 부정 승차 및 이상 징후를 감지하고 관리하는 지능형 통합 보안 시스템입니다.

---

## 🚀 주요 기능

*   **보안 로그인 시스템**: 라즈베리파이 내 MariaDB와 연동된 TCP/IP 소켓 통신을 통한 사용자 인증.
*   **멀티 프로토콜 스트리밍**: 라즈베리파이, 웹캠, 로컬 영상 파일로부터 **RTSP 실시간 영상** 수신 및 재생 지원.
    *   **저지연 최적화 (Low Latency)**: 별도의 스레드(`VideoCaptureWorker`)와 최소화된 버퍼링 설정을 통해 실시간 응답성 보장.
*   **실시간 영상 처리**: OpenCV 기반의 밝기 조절, ROI(관심 영역) 확대, AI 판독 결과 시각화.
*   **중앙 집중형 로그 관리**: 인증 시도 및 이상 징후 발생 시 서버 데이터베이스에 실시간 타임스탬프 기록.
*   **최신 UI/UX**: **Qt Quick (QML)**을 사용하여 유려하고 반응성이 뛰어난 다크 테마 인터페이스 구현.

---

## 🛠️ 시스템 요구 사항

### 클라이언트 환경 (Client)
*   **OS**: Windows 10/11
*   **Framework**: Qt 6.4 이상 (필수 구성 요소: `QtQuick`, `QtQuickControls2`)
*   **Compiler**: MinGW 64-bit (권장) 또는 MSVC
*   **Library**: OpenCV 4.5.5 (MinGW 빌드)
*   **Network Ports**:
    *   TCP Port `5555` (인증/Auth)
    *   RTSP Port `8554` (스트리밍/Streaming)

### 네트워크 설정 (필수)
클라이언트를 실행하기 전에 `src/authmanager.cpp`와 `src/videodisplayitem.cpp` 파일에서 서버(라즈베리파이)의 IP 주소가 올바르게 설정되어 있는지 확인하십시오.
*   기본 인증 서버 IP: `192.168.0.89`
*   기본 RTSP 주소: `rtsp://admin:CCgbdCCgbd@192.168.0.30/profile2/media.smp`

---

## ⚙️ 설정 및 빌드 방법

### 1. 사전 준비 (OpenCV)
이 프로젝트는 **OpenCV MinGW 빌드 (`OpenCV-4.5.5-x64`)**가 필요합니다.
프로젝트 루트의 **상위 디렉토리**에 다운로드하거나 `CMakeLists.txt`에서 경로를 수정해야 합니다.

```powershell
# 예시: 이 프로젝트의 상위 폴더에서 실행
git clone --branch OpenCV-4.5.5-x64 --depth 1 https://github.com/huihut/OpenCV-MinGW-Build.git OpenCV-MinGW-Build
```

### 2. 빌드 방법 (Command Line)

1.  **PowerShell** 또는 터미널을 엽니다.
2.  프로젝트 디렉토리로 이동합니다:
    ```powershell
    cd C:\path\to\qt_client_ui
    ```
3.  **빌드 디렉토리 생성**:
    ```powershell
    mkdir build-mingw
    cd build-mingw
    ```
4.  **CMake 구성 (Configure)**:
    ```powershell
    cmake -G "MinGW Makefiles" ..
    ```
5.  **빌드 (Build)**:
    ```powershell
    cmake --build .
    ```

### 3. 애플리케이션 실행
빌드가 성공하면 빌드 디렉토리 내의 실행 파일을 실행합니다:
```powershell
.\appHanwhaVisionSFEPS.exe
```

---

## 📂 프로젝트 구조

*   `src/`: 메인 소스 코드.
    *   `main.cpp`: 프로그램 진입점, QML 타입 등록.
    *   `authmanager.h/cpp`: TCP 로그인 로직 처리.
    *   `videodisplayitem.h/cpp`: RTSP 스트리밍 표시 (QQuickPaintedItem), 백그라운드 스레드 포함.
    *   `views/`: QML 뷰 파일들 (로그인, 모니터링 화면 등).
*   `assets/`: 이미지 및 아이콘 리소스.
*   `CMakeLists.txt`: 빌드 설정 파일.
