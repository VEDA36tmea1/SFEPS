# GStreamer laserdetect 플러그인 (상위 CMake에서 add_subdirectory로 포함)

find_package(OpenCV REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-video-1.0)

# 플러그인은 GStreamer가 로드하는 .so. 이름은 libgstlaserdetect.so
add_library(gstlaserdetect MODULE
    gstlaserdetect.c
    laserdetect_process.cpp
)

target_include_directories(gstlaserdetect PRIVATE
    ${OpenCV_INCLUDE_DIRS}
    ${GST_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(gstlaserdetect PRIVATE
    ibvs_core
    ${OpenCV_LIBS}
    ${GST_LIBRARIES}
)

set_target_properties(gstlaserdetect PROPERTIES
    PREFIX ""
    OUTPUT_NAME "gstlaserdetect"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/gst-plugin"
)
