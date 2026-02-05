# SFEPS - Subway Fare Evasion Prevention System

**SFEPS**는 Qt6 기반의 클라이언트와 라즈베리파이(MariaDB + GStreamer) 서버를 결합하여, 지하철 내 부정 승차 및 이상 징후를 실시간으로 감지하고 관리하는 지능형 통합 보안 시스템입니다.
본 클라이언트는 **Hanwha Vision** 스타일의 다크 테마와 모듈화된 View 아키텍처를 기반으로, 기존의 TCP 기반 사용자 인증 및 실시간 영상 제어 기능을 완벽히 통합하였습니다.

---

## 🚀 주요 기능
* **보안 로그인 시스템**: `QTcpSocket`을 통한 라즈베리파이 서버 연동 및 사용자 인증 ("ID:PW" 프로토콜).
* **현대적인 다크 테마 UI**: Hanwha Vision 디자인 가이드를 준수한 세련된 다크 모드 및 QSS 스타일 적용.
* **실시간 영상 스트리밍 및 제어**: 
    - **RTSP 실시간 피드** 수신 (OpenCV + FFMPEG 백엔드).
    - 실시간 **밝기(Brightness) 보정** 슬라이더 기능.
    - 마우스 드래그를 통한 **실시간 ROI 확대(Zoom)** 기능.
* **지능형 이벤트 로그 관리**: 
    - 실시간 이벤트 발생 시 사이드바 로그 기록.
    - 로그 클릭 시 **현장 스냅샷 및 상세 정보 팝업** 출력.
* **멀티 뷰 구조**: `QStackedWidget`을 활용한 대시보드 기반 화면 전환 (Monitoring, Analytics, Settings 등).

---

## 🛠️ 프로젝트 구조
```text
client/
├── src/
│   ├── main.cpp          # 애플리케이션 진입점 및 스타일시트 로드
│   ├── mainwindow.cpp    # 메인 윈도우 UI 구조 및 네비게이션 제어
│   └── views/            # 화면별 독립 컴포넌트
│       ├── LoginView      # TCP 기반 보안 로그인 화면
│       ├── MonitoringView # 실시간 스트리밍, 영상 제어 및 이벤트 로그 핵심 뷰
│       ├── DetailView     # 하드웨어 상세 상태
│       ├── AnalyticsView  # 통계 분석 데이터 시각화
│       └── SettingsView   # 시스템 설정
├── styles.qss            # 글로벌 스타일시트 (Hanwha Vision Theme)
└── resources.qrc         # Qt 리소스 파일 (스타일 및 정적 자원)
```

---

## 🛠️ 시스템 구성 및 요구 사항
### [Client]
* **OS**: Windows 10/11
* **Framework**: Qt 6.10.0 (MinGW 13.1.0 64-bit)
* **Library**: OpenCV 4.5.5 (MinGW 빌드본)
* **Network**: TCP Port 5555 (Auth), RTSP Port 8554 (Streaming)

---

## ⚙️ 네트워크 설정 (필수)
클라이언트 실행 전 아래 파일에서 서버(라즈베리파이)의 IP 주소를 확인하십시오.
- `src/views/LoginView.cpp`: 서버 인증용 IP (`192.168.0.89`)
- `src/views/MonitoringView.cpp`: RTSP 스트리밍 주소

---

## 1. 사전 준비 (OpenCV 설치)
이 프로젝트는 특정 버전의 OpenCV MinGW 빌드(`OpenCV-4.5.5-x64`)를 필요로 합니다.
`client` 폴더의 상위 디렉토리(프로젝트 루트)에 OpenCV를 다운로드해야 합니다.

```powershell
# 프로젝트 루트(SFEPS)에서 실행:
git clone --branch OpenCV-4.5.5-x64 --depth 1 https://github.com/huihut/OpenCV-MinGW-Build.git OpenCV-MinGW-Build
```

## 2. 프로젝트 구성 및 빌드
CMake를 사용하여 빌드를 구성합니다.

```powershell
# client 폴더 내에서 실행:

# 1. 구성 (Configure)
cmake -S . -B build

# 2. 빌드 (Build)
cmake --build build
```

빌드가 완료되면 `build` 폴더 내에 `QtVideoPlayer.exe` 실행 파일이 생성됩니다.
windeployqt와 OpenCV DLL 복사는 빌드 프로세스 중에 자동으로 수행됩니다.
