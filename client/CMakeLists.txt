cmake_minimum_required(VERSION 3.21)
project(QtVideoPlayer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_PREFIX_PATH "C:/Qt/6.10.0/mingw_64")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# [1] 패키지 찾기
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia MultimediaWidgets Network)

# [2] OpenCV 경로 설정
set(OPENCV_BIN_DIR "C:/Users/2-08/OpenCV-MinGW-Build/x64/mingw/bin")
set(OPENCV_INCLUDE_DIR "C:/Users/2-08/OpenCV-MinGW-Build/include")
set(OPENCV_LIB_DIR "C:/Users/2-08/OpenCV-MinGW-Build/x64/mingw/lib")

# --- [3] 핵심: 출력 경로를 프로젝트폴더/build 로 강제 지정 ---
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build")

# [4] 실행 파일 선언
add_executable(QtVideoPlayer
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/logindialog.cpp
    src/logindialog.h
)

target_include_directories(QtVideoPlayer SYSTEM PRIVATE "${OPENCV_INCLUDE_DIR}")

# [5] 라이브러리 연결
set(OPENCV_LIBS
    "${OPENCV_LIB_DIR}/libopencv_highgui455.dll.a"
    "${OPENCV_LIB_DIR}/libopencv_videoio455.dll.a"
    "${OPENCV_LIB_DIR}/libopencv_imgcodecs455.dll.a"
    "${OPENCV_LIB_DIR}/libopencv_imgproc455.dll.a"
    "${OPENCV_LIB_DIR}/libopencv_core455.dll.a"
)

target_link_libraries(QtVideoPlayer PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets Qt6::Network
    ${OPENCV_LIBS}
)

# --- [6] 자동 DLL 복사 스크립트 (build 폴더로 모으기) ---

# 1. OpenCV DLL 복사
add_custom_command(TARGET QtVideoPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${OPENCV_BIN_DIR}"
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMENT "Copying OpenCV DLLs to build directory..."
)

# 2. Qt DLL 자동 배포 (windeployqt)
find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "C:/Qt/6.10.0/mingw_64/bin")
if(WINDEPLOYQT_EXECUTABLE)
    add_custom_command(TARGET QtVideoPlayer POST_BUILD
        COMMAND "${WINDEPLOYQT_EXECUTABLE}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/QtVideoPlayer.exe"
        COMMENT "Running windeployqt in build directory..."
    )
endif()