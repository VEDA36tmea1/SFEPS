# 메타데이터 전용 테스트 & ESP 통신용 TCP 서버 빌드용 Makefile

CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra

CAMERA_SRCS := \
    camera_meta_test.cpp \
    rtsp_client_simple.cpp \
    bbox_to_esp.cpp

SERVER_SRCS := \
    raspi_tcp_server.cpp

TARGETS := camera_meta_test raspi_tcp_server

all: $(TARGETS)

camera_meta_test: $(CAMERA_SRCS)
	$(CXX) $(CXXFLAGS) $(CAMERA_SRCS) -o $@

raspi_tcp_server: $(SERVER_SRCS)
	$(CXX) $(CXXFLAGS) $(SERVER_SRCS) -o $@

clean:
	rm -f $(TARGETS)
