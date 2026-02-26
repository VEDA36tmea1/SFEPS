cmake_minimum_required(VERSION 3.10)
project(ibvs_laser_host_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-video-1.0)

# 공용 라이브러리: GStreamer 파이프라인/다른 앱에서 재사용 가능
add_library(ibvs_core STATIC
    src/vision_detector.cpp
    src/ibvs_controller.cpp
    src/stm_interface.cpp
    src/gst_ibvs.cpp
)

target_include_directories(ibvs_core PUBLIC
    ${OpenCV_INCLUDE_DIRS}
    ${GST_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ibvs_core PUBLIC
    ${OpenCV_LIBS}
    ${GST_LIBRARIES}
)

# 데모 실행 파일: ibvs_core 라이브러리를 사용
add_executable(ibvs_host
    src/main_ibvs.cpp
)

target_link_libraries(ibvs_host PRIVATE ibvs_core)

# RTSP 레이저 검출 데모 (OpenCV VideoCapture 사용)
add_executable(rtsp_laser_demo
    src/rtsp_laser_demo.cpp
)

target_link_libraries(rtsp_laser_demo PRIVATE ibvs_core)

