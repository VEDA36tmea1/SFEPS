# SFEPS - Subway Fare Evasion Prevention System

**SFEPS**는 Qt6 기반의 클라이언트와 라즈베리파이(MariaDB + GStreamer) 서버를 결합하여, 지하철 내 부정 승차 및 이상 징후를 실시간으로 감지하고 관리하는 지능형 통합 보안 시스템입니다.

---

## 🚀 주요 기능
* **보안 로그인 시스템**: TCP/IP 소켓 통신을 통해 라즈베리파이 내 MariaDB와 연동된 사용자 인증 기능.
* **멀티 프로토콜 스트리밍**: 라즈베리파이로부터의 **RTSP 실시간 피드** 송수신 및 웹캠, 로컬 영상 지원. **RTSP 스트리밍 지연 최적화**를 통해 실시간 성능 향상 (별도 스레드 기반 프레임 읽기 및 버퍼 최소화).
* **실시간 영상 처리**: OpenCV 기반의 실시간 밝기 조절, ROI(관심 영역) 확대 및 AI 판독 결과 시각화.
* **중앙 집중형 로그 관리**: 인증 시도 및 이상 징후 발생 시 서버(MariaDB)에 실시간 타임스탬프 기록.
* **사용자 인터랙션**: 마우스 드래그를 통한 특정 구역 확대 및 슬라이더를 이용한 영상 보정.

---

## ⚡ 성능 최적화
* **RTSP 스트리밍 지연 최소화**: OpenCV의 VideoCapture 버퍼를 1로 설정하고, 프레임 읽기를 별도 스레드(VideoCaptureWorker)에서 수행하여 메인 UI 블로킹을 방지. 이를 통해 실시간 영상 처리의 응답성을 향상시킵니다.

---

## 🛠️ 시스템 구성 및 요구 사항

### [Client]
* **OS**: Windows 10/11
* **Framework**: Qt 6.10.0 (MinGW 13.1.0 64-bit)
* **Library**: OpenCV 4.5.5 (MinGW 빌드본)
* **Network**: TCP Port 1234 (Auth), RTSP Port 8554 (Streaming)

---

## ⚙️ 네트워크 설정 (필수)
클라이언트 실행 전 `LoginDialog` 관련 코드에서 서버(라즈베리파이)의 IP 주소를 반드시 확인하십시오.

---

## 1. 사전 준비 (OpenCV 설치)
이 프로젝트는 특정 버전의 OpenCV MinGW 빌드(`OpenCV-4.5.5-x64`)를 필요로 합니다.
`client` 폴더의 상위 디렉토리(프로젝트 루트)에 OpenCV를 다운로드해야 합니다.

```powershell
# 프로젝트 루트(SFEPS)에서 실행:
git clone --branch OpenCV-4.5.5-x64 --depth 1 https://github.com/huihut/OpenCV-MinGW-Build.git OpenCV-MinGW-Build
```

## 2. 프로젝트 구성 및 빌드
MinGW Makefiles를 사용하여 빌드를 구성하고 실행합니다.

```powershell
# client 폴더 내에서 실행:

# 1. 구성 (Generatate)
cmake -G "MinGW Makefiles" -S . -B build

# 2. 빌드 (Build)
cmake --build build
```

빌드가 완료되면 `build` 폴더 내에 `QtVideoPlayer.exe` 실행 파일이 생성됩니다.