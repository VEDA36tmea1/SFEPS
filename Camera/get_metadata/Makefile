# 컴파일러 설정(기본: Linux/MinGW)
CXX = g++

ifeq ($(OS),Windows_NT)
DUMP_TARGET = dump_metadata_xml.exe
else
DUMP_TARGET = dump_metadata_xml
endif

# 플랫폼별 링커/플래그
ifeq ($(OS),Windows_NT)
# On Windows: MSVC(cl) + OpenCV(vc17) 사용
TARGET     = app.exe
CAM_TARGET = camera_client.exe
RBF_TARGET = camera_RBF.exe
# OpenCV Windows 패키지 루트(build 폴더 포함)
OPENCV_ROOT ?= C:/Users/2-16/Downloads/opencv-gst/install
OPENCV_LIB_DIR = $(OPENCV_ROOT)/x64/vc17/lib
OPENCV_BIN_DIR = $(OPENCV_ROOT)/x64/vc17/bin
GSTREAMER_ROOT ?= C:/Program Files/gstreamer/1.0/msvc_x86_64
GSTREAMER_BIN_DIR = $(GSTREAMER_ROOT)/bin
GSTREAMER_PLUGIN_DIR = $(GSTREAMER_ROOT)/lib/gstreamer-1.0

# MSVC 툴체인(Developer PowerShell/VS x64 Native Tools에서 실행 권장)
CXX = cl
CXXFLAGS = /std:c++17 /EHsc /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Iinc /I"$(OPENCV_ROOT)/include"
LDFLAGS = /link /LIBPATH:"$(OPENCV_LIB_DIR)"
LDLIBS = ws2_32.lib opencv_world4130.lib
ALL_TARGETS = $(TARGET) $(CAM_TARGET) $(RBF_TARGET)
CLEAN_CMD = cmd /C del /Q $(TARGET) $(CAM_TARGET) $(RBF_TARGET) $(DUMP_TARGET) app camera_client camera_RBF dump_metadata_xml 2>nul
else
TARGET     = app
CAM_TARGET = camera_client
RBF_TARGET = camera_RBF
LDLIBS =
LDFLAGS =
CXXFLAGS = -std=c++17 -Wall -I./inc
OPENCV_FLAGS = `pkg-config --cflags --libs opencv4`
ALL_TARGETS = $(TARGET) $(CAM_TARGET) $(RBF_TARGET)
CLEAN_CMD = rm -f $(TARGET) $(CAM_TARGET) $(RBF_TARGET) $(DUMP_TARGET)
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

# 5. 메타데이터 XML 덤프/오프라인 파싱 (OpenCV 불필요)
DUMP_SRCS = src/dump_metadata_xml.cpp \
            src/RTSPClient.cpp \
            src/XMLParser.cpp


# 빌드 규칙
all: $(TARGET) $(CAM_TARGET) $(RBF_TARGET) 

$(TARGET): $(SRCS)
ifeq ($(OS),Windows_NT)
	$(CXX) $(CXXFLAGS) $(SRCS) /Fe:$(TARGET) $(LDFLAGS) $(LDLIBS)
else
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET) $(LDLIBS)
endif

$(CAM_TARGET): $(CAM_SRCS)
ifeq ($(OS),Windows_NT)
	$(CXX) $(CXXFLAGS) $(CAM_SRCS) /Fe:$(CAM_TARGET) $(LDFLAGS) $(LDLIBS)
else
	$(CXX) $(CXXFLAGS) $(CAM_SRCS) -o $(CAM_TARGET) $(OPENCV_FLAGS) $(LDLIBS) -lpthread
endif

$(RBF_TARGET): $(RBF_SRCS)
ifeq ($(OS),Windows_NT)
	$(CXX) $(CXXFLAGS) $(RBF_SRCS) /Fe:$(RBF_TARGET) $(LDFLAGS) $(LDLIBS)
else
	$(CXX) $(CXXFLAGS) $(RBF_SRCS) -o $(RBF_TARGET) $(OPENCV_FLAGS) $(LDLIBS) -lpthread
endif

$(DUMP_TARGET): $(DUMP_SRCS)
ifeq ($(OS),Windows_NT)
	$(CXX) /std:c++17 /EHsc /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Iinc $(DUMP_SRCS) /Fe:$(DUMP_TARGET) /link ws2_32.lib
else
	$(CXX) -std=c++17 -Wall -I./inc $(DUMP_SRCS) -o $(DUMP_TARGET)
endif



clean:
	$(CLEAN_CMD)

# Windows 실행 헬퍼: OpenCV DLL 경로를 PATH에 추가해 실행
ARGS ?=
run-rbf: $(RBF_TARGET)
ifeq ($(OS),Windows_NT)
	cmd /C "set PATH=$(OPENCV_BIN_DIR);$(GSTREAMER_BIN_DIR);%PATH% && set GST_PLUGIN_PATH=$(GSTREAMER_PLUGIN_DIR) && .\$(RBF_TARGET) $(ARGS)"
else
	./$(RBF_TARGET) $(ARGS)
endif

# $(MAKE) 재귀 호출은 GnuWin32 등에서 make 경로에 공백이 있으면 cmd가 'C:/Program'만 실행해 실패한다.
run-rbf-native: $(RBF_TARGET)
ifeq ($(OS),Windows_NT)
	cmd /C "set PATH=$(OPENCV_BIN_DIR);$(GSTREAMER_BIN_DIR);%PATH% && set GST_PLUGIN_PATH=$(GSTREAMER_PLUGIN_DIR) && .\$(RBF_TARGET) --tracker-mode native"
else
	./$(RBF_TARGET) --tracker-mode native
endif

run-rbf-deepsort: $(RBF_TARGET)
ifeq ($(OS),Windows_NT)
	cmd /C "set PATH=$(OPENCV_BIN_DIR);$(GSTREAMER_BIN_DIR);%PATH% && set GST_PLUGIN_PATH=$(GSTREAMER_PLUGIN_DIR) && .\$(RBF_TARGET) --tracker-mode deepsort"
else
	./$(RBF_TARGET) --tracker-mode deepsort
endif
