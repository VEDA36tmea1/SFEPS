cmake_minimum_required(VERSION 3.10)
project(RpiSmartServer)

# C++ 표준 설정 (C++17 추천)
set(CMAKE_CXX_STANDARD 17)

# 1. 패키지 찾기 (GStreamer, MariaDB)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-rtsp-server-1.0)

# MariaDB 라이브러리 경로는 보통 시스템 경로에 있습니다.
set(MYSQL_LIBS "mysqlclient")

# 2. 헤더 파일 경로 포함
include_directories(include)

# 3. 실행 파일 생성 (main.cpp + db_handler.cpp)
add_executable(smart_server src/main.cpp src/db_handler.cpp)

# 4. 라이브러리 연결
target_link_libraries(smart_server ${GST_LIBRARIES} ${MYSQL_LIBS})
target_include_directories(smart_server PUBLIC ${GST_INCLUDE_DIRS})
