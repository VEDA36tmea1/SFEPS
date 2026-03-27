cmake_minimum_required(VERSION 3.16)

# Windows (Qt6 + MSVC): configure once, then build.
#   cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
#   cmake --build build-msvc --config Release
# OpenCV path (저지연 RTSP + 네이티브 트랙 + RBF/PWM, 기본 ON):
#   cmake ... -DOpenCV_DIR="C:/path/to/opencv-gst/install"
#   -DSFEPS_WITH_OPENCV=OFF  → Qt Multimedia만 (OpenCV 없이 빌드)
# Do not reuse a "build" directory that was configured for ../client (OpenCV);
# that CMakeLists is a different project and will break configure here.

project(HanwhaVisionSFEPS VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Qt6 찾기 (Quick, QuickControls2, Network, Multimedia 모듈 필수)
# 6.10+: VideoDisplay uses MediaPlayer.playbackOptions LowLatencyStreaming (FFmpeg RTSP).
find_package(Qt6 6.10 REQUIRED COMPONENTS Quick QuickControls2 Network Multimedia)

# Qt 리소스 추가
qt_add_resources(RESOURCES resources.qrc)

option(SFEPS_WITH_OPENCV "OpenCV low-latency RTSP + native track + RBF/PWM (needs OpenCV+optional GStreamer)" ON)

set(SFEPS_CLIENT_SOURCES
    src/main.cpp
    src/authmanager.cpp src/authmanager.h
    src/videobackend.cpp src/mainwindow.h
    src/voicemanager.cpp src/voicemanager.h
    src/fraudmanager.cpp src/fraudmanager.h
    src/positionmanager.cpp src/positionmanager.h
    src/pwmtransmitter.cpp src/pwmtransmitter.h
    src/videoarchivemanager.cpp src/videoarchivemanager.h
    src/recordinglistmodel.cpp src/recordinglistmodel.h
    src/live_frame_provider.cpp src/live_frame_provider.h
    ../Camera/get_metadata/src/RTSPClient.cpp
    ../Camera/get_metadata/src/XMLParser.cpp
    ${RESOURCES}
)

if(SFEPS_WITH_OPENCV)
    find_package(OpenCV REQUIRED COMPONENTS core imgproc videoio)
    list(APPEND SFEPS_CLIENT_SOURCES
        src/rbf_pwm_core.cpp src/rbf_pwm_core.h
        src/native_metadata_tracker.cpp src/native_metadata_tracker.h
    )
endif()

qt_add_executable(appHanwhaVisionSFEPS ${SFEPS_CLIENT_SOURCES})

target_include_directories(appHanwhaVisionSFEPS
    PRIVATE
    src
    ../Camera/get_metadata/inc
)

target_link_libraries(appHanwhaVisionSFEPS
    PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Network Qt6::Multimedia
)

if(SFEPS_WITH_OPENCV)
    target_compile_definitions(appHanwhaVisionSFEPS PRIVATE SFEPS_HAVE_OPENCV)
    target_link_libraries(appHanwhaVisionSFEPS PRIVATE ${OpenCV_LIBS})
endif()

# QML 리소스 설정
qt_add_qml_module(appHanwhaVisionSFEPS
    URI "HanwhaVisionSFEPS"
    VERSION 1.0
    QML_FILES
        src/Main.qml
        src/AppTheme.qml
        src/VideoDisplay.qml
        src/views/LoginView.qml
        src/views/MonitoringView.qml
        src/views/DetailView.qml
        src/views/AnalyticsView.qml
        src/views/ArchiveView.qml
        src/views/SettingsView.qml
)

# 편의를 위해 자산 폴더를 빌드 디렉토리로 복사 (배포 방식에 따라 선택 사항)
file(COPY assets DESTINATION ${CMAKE_BINARY_DIR})

install(TARGETS appHanwhaVisionSFEPS
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
