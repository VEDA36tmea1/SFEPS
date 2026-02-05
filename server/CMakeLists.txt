cmake_minimum_required(VERSION 3.10)
project(RpiSmartServer)

# 1. C++ 표준 설정 (파일 시스템 및 최신 기능 사용을 위해 C++17 필수)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 2. 패키지 찾기 도구 로드
find_package(PkgConfig REQUIRED)

# 3. 필수 라이브러리 찾기
# 3-1. GStreamer (기본, RTSP 서버, AppSink 라이브러리 모두 포함)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-rtsp-server-1.0 gstreamer-app-1.0)

# 3-2. MariaDB (데이터베이스)
pkg_check_modules(MARIADB REQUIRED mariadb)

# 3-3. TinyXML2 (메타데이터 XML 파싱)
pkg_check_modules(TINYXML2 REQUIRED tinyxml2)

# 3-4. Threads (비동기 로그 처리를 위한 스레드)
find_package(Threads REQUIRED)

# 4. 헤더 파일 경로 설정 (include 폴더)
include_directories(include)

# 5. 실행 파일 생성 (소스 파일 목록: main.cpp, log.cpp)
add_executable(smart_server src/main.cpp src/log.cpp)

# 6. 라이브러리 연결 (Link)
# 찾은 라이브러리들을 실행 파일에 붙입니다.
target_link_libraries(smart_server 
    ${GST_LIBRARIES} 
    ${MARIADB_LIBRARIES} 
    ${TINYXML2_LIBRARIES}
    Threads::Threads
)

# 7. 라이브러리 헤더 경로 포함 (Include)
# 라이브러리의 헤더 파일(.h) 위치를 알려줍니다.
target_include_directories(smart_server PUBLIC 
    ${GST_INCLUDE_DIRS} 
    ${MARIADB_INCLUDE_DIRS}
    ${TINYXML2_INCLUDE_DIRS}
)