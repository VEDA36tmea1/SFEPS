# ================================================================
# Makefile — camera_RBF / camera_client / app / dump_metadata_xml
#
# Windows : MSVC  → make_msvc.cmd 으로 실행 (VS Developer 환경 자동 설정)
# Linux   : g++ + pkg-config (opencv4)
# ================================================================
ARGS ?=

# ----------------------------------------------------------------
# Windows (MSVC)
# ----------------------------------------------------------------
ifeq ($(OS),Windows_NT)

BUILD_DIR         = obj-msvc
OPENCV_ROOT      ?= C:/Users/2-16/Downloads/opencv-gst/install
GSTREAMER_ROOT   ?= C:/Program Files/gstreamer/1.0/msvc_x86_64
OPENCV_LIB_DIR    = $(OPENCV_ROOT)/x64/vc17/lib
OPENCV_BIN_DIR    = $(OPENCV_ROOT)/x64/vc17/bin
GSTREAMER_BIN_DIR = $(GSTREAMER_ROOT)/bin
GSTREAMER_PLUGIN  = $(GSTREAMER_ROOT)/lib/gstreamer-1.0
OPENCV_WORLD_LIB ?= opencv_world4130.lib

CXX      = cl
CXXFLAGS = /std:c++17 /EHsc /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS \
           /Iinc /I"$(OPENCV_ROOT)/include" \
           /Fo$(BUILD_DIR)/
LDFLAGS  = /link /LIBPATH:"$(OPENCV_LIB_DIR)"
LDLIBS   = ws2_32.lib $(OPENCV_WORLD_LIB)

TARGET      = $(BUILD_DIR)/app.exe
CAM_TARGET  = $(BUILD_DIR)/camera_client.exe
RBF_TARGET  = $(BUILD_DIR)/camera_RBF.exe
DUMP_TARGET = $(BUILD_DIR)/dump_metadata_xml.exe

# ----------------------------------------------------------------
# Linux (GCC)
# ----------------------------------------------------------------
else

CXX      = g++
CXXFLAGS = -std=c++17 -Wall -I./inc
LDLIBS   =
LDFLAGS  =

TARGET      = app
CAM_TARGET  = camera_client
RBF_TARGET  = camera_RBF
DUMP_TARGET = dump_metadata_xml

endif

# ================================================================
# 소스 목록
# ================================================================
SRCS      = src/main.cpp src/RTSPClient.cpp src/XMLParser.cpp
CAM_SRCS  = src/camera_client.cpp src/RTSPClient.cpp src/XMLParser.cpp src/re_id.cpp
RBF_SRCS  = src/camera_RBF.cpp src/RTSPClient.cpp src/XMLParser.cpp src/re_id.cpp
DUMP_SRCS = src/dump_metadata_xml.cpp src/RTSPClient.cpp src/XMLParser.cpp

# ================================================================
.PHONY: all clean run-rbf run-rbf-native run-rbf-deepsort \
        rbf cam app rbf-only

all: $(TARGET) $(CAM_TARGET) $(RBF_TARGET)

rbf: $(RBF_TARGET)
cam: $(CAM_TARGET)
app: $(TARGET)
rbf-only: $(RBF_TARGET)

# ----------------------------------------------------------------
# Windows 빌드 규칙
# ----------------------------------------------------------------
ifeq ($(OS),Windows_NT)

$(TARGET): $(SRCS)
	cmd /C "if not exist $(BUILD_DIR) mkdir $(BUILD_DIR)"
	$(CXX) $(CXXFLAGS) $(SRCS) /Fe:$(TARGET) $(LDFLAGS) $(LDLIBS)

$(CAM_TARGET): $(CAM_SRCS)
	cmd /C "if not exist $(BUILD_DIR) mkdir $(BUILD_DIR)"
	$(CXX) $(CXXFLAGS) $(CAM_SRCS) /Fe:$(CAM_TARGET) $(LDFLAGS) $(LDLIBS)

$(RBF_TARGET): $(RBF_SRCS)
	cmd /C "if not exist $(BUILD_DIR) mkdir $(BUILD_DIR)"
	$(CXX) $(CXXFLAGS) $(RBF_SRCS) /Fe:$(RBF_TARGET) $(LDFLAGS) $(LDLIBS)

$(DUMP_TARGET): $(DUMP_SRCS)
	cmd /C "if not exist $(BUILD_DIR) mkdir $(BUILD_DIR)"
	$(CXX) /std:c++17 /EHsc /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Iinc \
	       /Fo$(BUILD_DIR)/ $(DUMP_SRCS) /Fe:$(DUMP_TARGET) /link ws2_32.lib

clean:
	cmd /C "if exist $(BUILD_DIR) (del /Q $(BUILD_DIR)\*.exe $(BUILD_DIR)\*.obj 2>nul)"

# ----------------------------------------------------------------
# Linux 빌드 규칙
# ----------------------------------------------------------------
else

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET) $(LDLIBS)

$(CAM_TARGET): $(CAM_SRCS)
	$(CXX) $(CXXFLAGS) $(CAM_SRCS) -o $(CAM_TARGET) \
	       `pkg-config --cflags --libs opencv4` $(LDLIBS) -lpthread

$(RBF_TARGET): $(RBF_SRCS)
	$(CXX) $(CXXFLAGS) $(RBF_SRCS) -o $(RBF_TARGET) \
	       `pkg-config --cflags --libs opencv4` $(LDLIBS) -lpthread

$(DUMP_TARGET): $(DUMP_SRCS)
	$(CXX) -std=c++17 -Wall -I./inc $(DUMP_SRCS) -o $(DUMP_TARGET)

clean:
	rm -f $(TARGET) $(CAM_TARGET) $(RBF_TARGET) $(DUMP_TARGET)

endif

# ================================================================
# 실행 헬퍼 (Windows: OpenCV·GStreamer DLL 경로를 PATH에 추가)
# ================================================================
run-rbf: $(RBF_TARGET)
ifeq ($(OS),Windows_NT)
	@cmd /C "set \"PATH=$(OPENCV_BIN_DIR);$(GSTREAMER_BIN_DIR);%PATH%\" && \
	         set \"GST_PLUGIN_PATH=$(GSTREAMER_PLUGIN)\" && \
	         set \"USE_GSTREAMER=1\" && \
	         $(subst /,\,$(RBF_TARGET)) $(ARGS)"
else
	@./$(RBF_TARGET) $(ARGS)
endif

run-rbf-native: $(RBF_TARGET)
ifeq ($(OS),Windows_NT)
	@cmd /C "set \"PATH=$(OPENCV_BIN_DIR);$(GSTREAMER_BIN_DIR);%PATH%\" && \
	         set \"GST_PLUGIN_PATH=$(GSTREAMER_PLUGIN)\" && \
	         set \"USE_GSTREAMER=1\" && \
	         $(subst /,\,$(RBF_TARGET)) $(ARGS) --tracker-mode native"
else
	@./$(RBF_TARGET) $(ARGS) --tracker-mode native
endif

run-rbf-deepsort: $(RBF_TARGET)
ifeq ($(OS),Windows_NT)
	@cmd /C "set \"PATH=$(OPENCV_BIN_DIR);$(GSTREAMER_BIN_DIR);%PATH%\" && \
	         set \"GST_PLUGIN_PATH=$(GSTREAMER_PLUGIN)\" && \
	         set \"USE_GSTREAMER=1\" && \
	         $(subst /,\,$(RBF_TARGET)) $(ARGS) --tracker-mode deepsort"
else
	@./$(RBF_TARGET) $(ARGS) --tracker-mode deepsort
endif
