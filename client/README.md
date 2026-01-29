# 🛡️ MetroGuard AI - Intelligent Surveillance System

**MetroGuard AI**는 Qt6와 OpenCV를 결합하여 실시간 지하철 내 이상 징후를 감지하고 모니터링하기 위한 지능형 영상 감시 시스템 프로토타입입니다.

---

## 🚀 주요 기능
* **실시간 스트리밍**: RTSP 주소, 웹캠(0), 또는 로컬 영상 파일을 통한 실시간 피드 분석.
* **영상 처리**: OpenCV를 이용한 실시간 밝기 조절 및 ROI(관심 영역) 확대 기능.
* **지능형 로그**: 감지된 이상 징후의 타임스탬프 및 상세 내역(AI 판독 결과 등) 기록.
* **사용자 인터랙션**: 마우스 드래그를 통한 특정 구역 확대 및 슬라이더를 이용한 영상 보정.

---

## 🛠️ 개발 환경 및 요구 사항
* **OS**: Windows 10/11
* **Framework**: Qt 6.10.0 (MinGW 13.1.0 64-bit)
* **Library**: OpenCV 4.5.5 (MinGW 빌드본)
* **Build Tool**: CMake 3.21 이상

---

## 📦 빌드 및 설치 방법

### 1. 필수 경로 확인
빌드 전 다음 경로에 라이브러리가 설치되어 있는지 확인하십시오. (경로가 다를 경우 `CMakeLists.txt` 수정 필요)
* **OpenCV**: `C:/Users/2-08/OpenCV-MinGW-Build`
* **Qt**: `C:/Qt/6.10.0/mingw_64`

### 2. CMake 빌드 단계
PowerShell 또는 터미널에서 다음 명령어를 실행합니다.

```powershell
# 1. 빌드 폴더 생성 및 이동
mkdir build
cd build

# 2. CMake 구성 (MinGW 컴파일러 명시)
cmake -G "MinGW Makefiles" `
  -DCMAKE_C_COMPILER="C:/Qt/Tools/mingw1310_64/bin/gcc.exe" `
  -DCMAKE_CXX_COMPILER="C:/Qt/Tools/mingw1310_64/bin/g++.exe" `
  -DCMAKE_MAKE_PROGRAM="C:/Qt/Tools/mingw1310_64/bin/mingw32-make.exe" ..

# 3. 컴파일 및 빌드
cmake --build .