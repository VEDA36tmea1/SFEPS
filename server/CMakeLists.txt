cmake_minimum_required(VERSION 3.10)
project(SFEPS_Server)

set(CMAKE_CXX_STANDARD 17)

# 1. 실행 파일 생성 (cleanup.cpp 추가!)
add_executable(smart_server 
    src/main.cpp 
    src/log.cpp
    src/recorder.cpp
    src/cleanup.cpp
)

# 2. 헤더 파일 경로 설정
target_include_directories(smart_server PRIVATE 
    /usr/include/mysql
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 3. 라이브러리 링크
target_link_libraries(smart_server PRIVATE
    avformat
    avcodec
    avutil
    swscale
    mysqlclient
    tinyxml2
    pthread
    stdc++fs
)