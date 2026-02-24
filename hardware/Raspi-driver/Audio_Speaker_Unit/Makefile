TARGET   := audio_speaker_test
CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
CPPFLAGS := -I include
LDFLAGS  := -lasound -pthread

# 소스는 src/ 에 있음
SRC_DIR  := src
SRCS     := main.cpp \
            audio_ring_buffer.cpp \
            audio_playback.cpp

VPATH    := $(SRC_DIR)
OBJS     := $(SRCS:.cpp=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
