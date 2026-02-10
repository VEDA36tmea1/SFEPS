cmake_minimum_required(VERSION 3.16)

project(HanwhaVisionSFEPS VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Qt6 찾기 (Quick, QuickControls2, Network 모듈 필수)
find_package(Qt6 6.4 REQUIRED COMPONENTS Quick QuickControls2 Network)

# OpenCV 설정 (상위 디렉토리나 시스템 경로에 있다고 가정)
# 찾지 못하면 에러 발생
set(OpenCV_DIR "C:/Users/2-08/OpenCV-MinGW-Build")
find_package(OpenCV REQUIRED)

# Qt 리소스 추가
qt_add_resources(RESOURCES resources.qrc)

qt_add_executable(appHanwhaVisionSFEPS
    src/main.cpp
    src/authmanager.cpp src/authmanager.h
    src/mainwindow.cpp src/mainwindow.h
    ${RESOURCES}
)

target_link_libraries(appHanwhaVisionSFEPS
    PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Network
    ${OpenCV_LIBS}
)

# QML 리소스 설정
qt_add_qml_module(appHanwhaVisionSFEPS
    URI "HanwhaVisionSFEPS"
    VERSION 1.0
    QML_FILES
        src/Main.qml
        src/AppTheme.qml
        src/views/LoginView.qml
        src/views/MonitoringView.qml
        src/views/DetailView.qml
        src/views/AnalyticsView.qml
        src/views/SettingsView.qml
)

# 편의를 위해 자산 폴더를 빌드 디렉토리로 복사 (배포 방식에 따라 선택 사항)
file(COPY assets DESTINATION ${CMAKE_BINARY_DIR})

install(TARGETS appHanwhaVisionSFEPS
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
