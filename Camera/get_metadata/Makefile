# 컴파일러 설정
CXX = g++

# 1. 헤더 파일 경로
CXXFLAGS = -std=c++17 -Wall -I./inc

# 플랫폼별 링커/플래그
ifeq ($(OS),Windows_NT)
# On Windows (MSYS2/MinGW/MinGW-w64): link Winsock
TARGET     = app.exe
CAM_TARGET = camera_client.exe
RBF_TARGET = camera_RBF.exe
LDLIBS = -lws2_32
# MinGW용 OpenCV 루트 경로 (필요 시 make OPENCV_ROOT=... 로 덮어쓰기)
# 주의: g++ 빌드는 x64/vc16(MSVC) OpenCV와 ABI가 달라 링크되지 않는다.
OPENCV_ROOT ?= ../../OpenCV-MinGW-Build
OPENCV_FLAGS = -I$(OPENCV_ROOT)/include -L$(OPENCV_ROOT)/x64/mingw/lib \
       -lopencv_core455 -lopencv_highgui455 -lopencv_imgproc455 -lopencv_videoio455 -lopencv_imgcodecs455
ALL_TARGETS = $(TARGET) $(CAM_TARGET) $(RBF_TARGET)
CLEAN_CMD = cmd /C del /Q $(TARGET) $(CAM_TARGET) $(RBF_TARGET) app camera_client camera_RBF 2>nul
else
TARGET     = app
CAM_TARGET = camera_client
RBF_TARGET = camera_RBF
LDLIBS =
OPENCV_FLAGS = `pkg-config --cflags --libs opencv4`
ALL_TARGETS = $(TARGET) $(CAM_TARGET) $(RBF_TARGET)
CLEAN_CMD = rm -f $(TARGET) $(CAM_TARGET)
endif

# 2. 기본 앱용 소스 (기존 tailgating 분석 등)
SRCS = src/main.cpp \
       src/RTSPClient.cpp \
       src/XMLParser.cpp

# 3. camera_client (영상 + 메타데이터 + 바운딩 박스 클릭)
CAM_SRCS = src/camera_client.cpp \
           src/RTSPClient.cpp \
           src/XMLParser.cpp

# 4. camera_RBF (선택 bbox + ratio -> RBF(PWM) -> UDP 송신)
RBF_SRCS = src/camera_RBF.cpp \
           src/RTSPClient.cpp \
           src/XMLParser.cpp



# 빌드 규칙
all: $(TARGET) $(CAM_TARGET) $(RBF_TARGET) 

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET) $(LDLIBS)

$(CAM_TARGET): $(CAM_SRCS)
	$(CXX) $(CXXFLAGS) $(CAM_SRCS) -o $(CAM_TARGET) $(OPENCV_FLAGS) $(LDLIBS) -lpthread

$(RBF_TARGET): $(RBF_SRCS)
	$(CXX) $(CXXFLAGS) $(RBF_SRCS) -o $(RBF_TARGET) $(OPENCV_FLAGS) $(LDLIBS) -lpthread



clean:
	$(CLEAN_CMD)
