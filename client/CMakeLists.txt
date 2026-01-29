cmake_minimum_required(VERSION 3.21)
project(QtVideoPlayer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_PREFIX_PATH "C:/Qt/6.10.0/mingw_64")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# [1] 패키지 찾기
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia MultimediaWidgets)

# [2] OpenCV 경로 설정 (사용자님 경로에 딱 맞춤)
# 폴더 구조가 include/opencv2 형태이므로 include까지만 잡습니다.
set(OPENCV_INCLUDE_DIR "C:/Users/2-08/OpenCV-MinGW-Build/include")
set(OPENCV_LIB_DIR "C:/Users/2-08/OpenCV-MinGW-Build/x64/mingw/lib")

# [3] 실행 파일 선언
add_executable(QtVideoPlayer
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
)

# [4] 핵심 해결책: MOC와 컴파일러 모두에게 경로 강제 주입
# SYSTEM 키워드를 쓰면 컴파일러가 최우선으로 찾습니다.
target_include_directories(QtVideoPlayer SYSTEM PRIVATE "${OPENCV_INCLUDE_DIR}")

# [5] 라이브러리 연결 (숫자가 455인지 다시 확인 필수!)
set(OPENCV_LIBS
    "${OPENCV_LIB_DIR}/libopencv_highgui455.dll.a"
    "${OPENCV_LIB_DIR}/libopencv_videoio455.dll.a"
    "${OPENCV_LIB_DIR}/libopencv_imgcodecs455.dll.a"
    "${OPENCV_LIB_DIR}/libopencv_imgproc455.dll.a"
    "${OPENCV_LIB_DIR}/libopencv_core455.dll.a"
)

target_link_libraries(QtVideoPlayer PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets
    ${OPENCV_LIBS}
)