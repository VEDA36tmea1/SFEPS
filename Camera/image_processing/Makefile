# 컴파일러 및 플래그 설정
CXX = g++
CXXFLAGS = -Wall -O2 -std=c++11 -pthread -Iinc -I/usr/include/opencv4
LDFLAGS = $(shell pkg-config --cflags --libs opencv4) -pthread

# 타겟 실행 파일 이름
TARGET = camera_client

# 디렉토리 설정
SRCDIR = src
INCDIR = inc
OBJDIR = obj

# 소스 및 오브젝트 파일 자동 인식
SOURCES = $(wildcard $(SRCDIR)/*.cpp)
OBJECTS = $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$(SOURCES))

# 기본 타겟
all: $(TARGET)

# 타겟 실행 파일 링킹
$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

# 개별 소스 파일을 오브젝트 파일로 컴파일 (obj 폴더 자동 생성)
$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 빌드 찌꺼기 청소
clean:
	rm -rf $(OBJDIR) $(TARGET)
