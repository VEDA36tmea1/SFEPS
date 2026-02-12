TARGET := audio_speaker_test
CXX    := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
LDFLAGS  := -lasound -pthread

SRCS := main.cpp \
        audio_ring_buffer.cpp \
        audio_playback.cpp

OBJS := $(SRCS:.cpp=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

