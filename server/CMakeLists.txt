cmake_minimum_required(VERSION 3.10)
project(SFEPS_Server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. pkg-config 로드
find_package(PkgConfig REQUIRED)

# 2. 외부 라이브러리 탐색
pkg_check_modules(MARIADB REQUIRED mariadb)
pkg_check_modules(TINYXML2 REQUIRED tinyxml2)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale)
find_package(Threads REQUIRED)

# 3. 실행 파일
add_executable(smart_server 
    src/main.cpp 
    src/log.cpp
    src/recorder.cpp
    src/cleanup.cpp
    src/auth.cpp
    src/rfid_monitor.cpp
)

# 4. 헤더 경로
target_include_directories(smart_server PRIVATE
    ${MARIADB_INCLUDE_DIRS}
    ${TINYXML2_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 5. 라이브러리 링크
target_link_libraries(smart_server PRIVATE
    ${MARIADB_LIBRARIES}
    ${TINYXML2_LIBRARIES}
    ${FFMPEG_LIBRARIES}
    Threads::Threads
)
