# 컴파일러 설정
CXX = g++

# 1. 헤더 파일 경로
CXXFLAGS = -std=c++17 -Wall -I./inc \

# 2. 라이브러리 링크 (pkg-config 없이 직접 지정)
# -L... : 라이브러리 파일이 있는 폴더 (라즈베리파이 표준 경로)
LDFLAGS = -L/usr/lib/arm-linux-gnueabihf \
          -lmariadb -pthread

# 3. 소스 파일 목록 (비디오 관련 파일 제외)
SRCS = src/main.cpp \
       src/RTSPClient.cpp \
       src/XMLParser.cpp \

# 결과물 이름
TARGET = app

# 빌드 규칙
all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)
