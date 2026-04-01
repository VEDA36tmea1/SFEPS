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
    # OpenCV_DIR 미지정 시 프로젝트 루트(SFEPS/) 기준으로 자동 탐색
    # → 개발자마다 별도로 -DOpenCV_DIR=... 을 지정할 필요 없음
    if(NOT OpenCV_DIR OR OpenCV_DIR STREQUAL "OpenCV_DIR-NOTFOUND")
        set(_opencv_search_paths
            "${CMAKE_SOURCE_DIR}/../opencv-gst/install"
            "${CMAKE_SOURCE_DIR}/../opencv-gst/opencv-gst/install"
            "${CMAKE_SOURCE_DIR}/../opencv/build"
            "${CMAKE_SOURCE_DIR}/../opencv-build"
        )
        foreach(_p ${_opencv_search_paths})
            if(EXISTS "${_p}/OpenCVConfig.cmake")
                set(OpenCV_DIR "${_p}" CACHE PATH "Auto-detected OpenCV install dir" FORCE)
                message(STATUS "[SFEPS] OpenCV_DIR auto-detected: ${OpenCV_DIR}")
                break()
            endif()
        endforeach()
    endif()

    find_package(OpenCV REQUIRED COMPONENTS core imgproc videoio)
    list(APPEND SFEPS_CLIENT_SOURCES
        # camera_RBF.cpp: CAMERA_RBF_QT_MODE 정의 시 main()/GUI/소켓 제거
        # RbfTps2D / KalmanBbox2D / NativeTrack / IdStabilizer 등 모두 포함
        # → rbf_pwm_core.cpp / native_metadata_tracker.cpp 불필요
        ../Camera/get_metadata/src/camera_RBF.cpp
        ../Camera/get_metadata/src/re_id.cpp
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
    target_compile_definitions(appHanwhaVisionSFEPS PRIVATE
        SFEPS_HAVE_OPENCV
        CAMERA_RBF_QT_MODE   # camera_RBF.cpp: main()/GUI/소켓 제거, rbfqt_* 인터페이스 활성화
    )
    target_link_libraries(appHanwhaVisionSFEPS PRIVATE ${OpenCV_LIBS})
    target_include_directories(appHanwhaVisionSFEPS PRIVATE
        ../Camera/get_metadata/src   # camera_RBF.cpp 내부 include 경로 (Config.h 등)
    )
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
