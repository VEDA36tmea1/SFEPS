# 컴파일러 설정
CXX = g++

# 1. 헤더 파일 경로
CXXFLAGS = -std=c++17 -Wall -I./inc

# OpenCV 링크 플래그 (시스템에 따라 opencv4 / opencv 선택)
OPENCV_FLAGS = `pkg-config --cflags --libs opencv4`

# 2. 기본 앱용 소스 (기존 tailgating 분석 등)
SRCS = src/main.cpp \
       src/RTSPClient.cpp \
       src/XMLParser.cpp

# 3. camera_client (영상 + 메타데이터 + 바운딩 박스 클릭)
CAM_SRCS = src/camera_client.cpp \
           src/RTSPClient.cpp \
           src/XMLParser.cpp

# 결과물 이름
TARGET     = app
CAM_TARGET = camera_client

# 빌드 규칙
all: $(TARGET) $(CAM_TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET)

$(CAM_TARGET): $(CAM_SRCS)
	$(CXX) $(CXXFLAGS) $(CAM_SRCS) -o $(CAM_TARGET) $(OPENCV_FLAGS)

clean:
	rm -f $(TARGET) $(CAM_TARGET)
