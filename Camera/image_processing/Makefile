CXX      = g++
CXXFLAGS = -Wall -O2 -std=c++11 -pthread -Iinc -I/usr/include/opencv4
LDFLAGS  = $(shell pkg-config --cflags --libs opencv4) -pthread

TARGET = camera_client
SRCDIR = src
OBJDIR = obj

SOURCES = $(SRCDIR)/camera_client.cpp $(SRCDIR)/img_processing.cpp
OBJECTS = $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$(SOURCES))

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $^ -o $@ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(TARGET)