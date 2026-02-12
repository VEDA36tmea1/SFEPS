TARGET := send_audio
CXX    := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
LDFLAGS  :=

# Windows(Win32, MinGW 등)에서는 Winsock 라이브러리 링크 필요
ifeq ($(OS),Windows_NT)
LDFLAGS += -lws2_32
endif

SRCS := send_audio.cpp
OBJS := $(SRCS:.cpp=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

