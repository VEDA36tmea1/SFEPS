cmake_minimum_required(VERSION 3.21)
project(QtVideoPlayer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_PREFIX_PATH "C:/Qt/6.10.0/mingw_64")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# [1] Find Packages
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia MultimediaWidgets Network Concurrent)

# [2] OpenCV Configuration
set(OPENCV_BIN_DIR "${CMAKE_SOURCE_DIR}/../OpenCV-MinGW-Build/x64/mingw/bin")
set(OPENCV_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../OpenCV-MinGW-Build/include")
set(OPENCV_LIB_DIR "${CMAKE_SOURCE_DIR}/../OpenCV-MinGW-Build/x64/mingw/lib")

# --- [3] Output Output ---
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build")

# [4] Executable
add_executable(QtVideoPlayer
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    resources.qrc
    
    # Views
    src/views/LoginView.cpp
    src/views/LoginView.h
    src/views/MonitoringView.cpp
    src/views/MonitoringView.h
    src/views/DetailView.cpp
    src/views/DetailView.h
    src/views/SettingsView.cpp
    src/views/SettingsView.h
    src/views/AnalyticsView.cpp
    src/views/AnalyticsView.h
)

target_include_directories(QtVideoPlayer SYSTEM PRIVATE "${OPENCV_INCLUDE_DIR}")
target_include_directories(QtVideoPlayer PRIVATE "${CMAKE_SOURCE_DIR}/src")

# [5] Libraries
set(OPENCV_LIBS
    "${OPENCV_LIB_DIR}/libopencv_highgui455.dll.a"
    "${OPENCV_LIB_DIR}/libopencv_videoio455.dll.a"
    "${OPENCV_LIB_DIR}/libopencv_imgcodecs455.dll.a"
    "${OPENCV_LIB_DIR}/libopencv_imgproc455.dll.a"
    "${OPENCV_LIB_DIR}/libopencv_core455.dll.a"
)

target_link_libraries(QtVideoPlayer PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets Qt6::Network Qt6::Concurrent
    ${OPENCV_LIBS}
)

# --- [6] Post Build Copy ---

# 1. OpenCV DLLs
add_custom_command(TARGET QtVideoPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${OPENCV_BIN_DIR}"
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMENT "Copying OpenCV DLLs to build directory..."
)

# 2. windeployqt
find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "C:/Qt/6.10.0/mingw_64/bin")
if(WINDEPLOYQT_EXECUTABLE)
    add_custom_command(TARGET QtVideoPlayer POST_BUILD
        COMMAND "${WINDEPLOYQT_EXECUTABLE}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/QtVideoPlayer.exe"
        COMMENT "Running windeployqt in build directory..."
    )
endif()