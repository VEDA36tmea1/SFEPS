# Ubuntu 노트북용 ESP TCP 서버 빌드 Makefile

CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra

SERVER_SRCS := \
    ubuntu_tcp_server.cpp

TARGETS := ubuntu_tcp_server

all: $(TARGETS)

ubuntu_tcp_server: $(SERVER_SRCS)
	$(CXX) $(CXXFLAGS) $(SERVER_SRCS) -o $@

clean:
	rm -f $(TARGETS)