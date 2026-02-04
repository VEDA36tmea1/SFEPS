# SFEPS - Subway Fare Evasion Detection System

**SFEPS**는 Qt6 기반의 클라이언트와 라즈베리파이(MariaDB + GStreamer) 서버를 결합하여, 지하철 내 부정 승차 및 이상 징후를 실시간으로 감지하고 관리하는 지능형 통합 보안 시스템입니다.

---

## 🚀 주요 기능
* **보안 로그인 시스템**: TCP/IP 소켓 통신을 통해 라즈베리파이 내 MariaDB와 연동된 사용자 인증 기능.
* **멀티 프로토콜 스트리밍**: 라즈베리파이로부터의 **RTSP 실시간 피드** 송수신 및 웹캠, 로컬 영상 지원.
* **실시간 영상 처리**: OpenCV 기반의 실시간 밝기 조절, ROI(관심 영역) 확대 및 AI 판독 결과 시각화.
* **중앙 집중형 로그 관리**: 인증 시도 및 이상 징후 발생 시 서버(MariaDB)에 실시간 타임스탬프 기록.
* **사용자 인터랙션**: 마우스 드래그를 통한 특정 구역 확대 및 슬라이더를 이용한 영상 보정.

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

```cpp
// 서버 접속 정보 예시 (실제 라즈베리파이 IP로 수정 필요)
socket->connectToHost("192.168.0.92", 1234);

## 1. 빌드 폴더 생성 및 이동
mkdir build
cd build

## 2. 빌드 환경 구성
cmake -G "MinGW Makefiles" .. `
  -DCMAKE_C_COMPILER="C:/Qt/Tools/mingw1310_64/bin/gcc.exe" `
  -DCMAKE_CXX_COMPILER="C:/Qt/Tools/mingw1310_64/bin/g++.exe" `
  -DCMAKE_MAKE_PROGRAM="C:/Qt/Tools/mingw1310_64/bin/mingw32-make.exe"

## 3. 컴파일 및 빌드
cmake --build .