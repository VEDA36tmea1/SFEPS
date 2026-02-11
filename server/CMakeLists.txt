cmake_minimum_required(VERSION 3.10)
project(CCgbdServer)

set(CMAKE_CXX_STANDARD 17)

# 1. 패키지 찾기
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-rtsp-server-1.0)
pkg_check_modules(MARIADB REQUIRED mariadb)
find_package(Threads REQUIRED) # 스레드 필수

# 2. 헤더 파일 경로 (include 폴더)
include_directories(include)

# 3. 실행 파일 생성
add_executable(smart_server src/main.cpp src/log.cpp src/auth.cpp)

# 4. 라이브러리 연결
target_link_libraries(smart_server 
    ${GST_LIBRARIES} 
    ${MARIADB_LIBRARIES} 
    Threads::Threads
)

target_include_directories(smart_server PUBLIC 
    ${GST_INCLUDE_DIRS} 
    ${MARIADB_INCLUDE_DIRS}
)