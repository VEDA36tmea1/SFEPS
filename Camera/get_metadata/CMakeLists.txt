cmake_minimum_required(VERSION 3.16)

project(get_metadata_tools VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED OpenCV_DIR)
    set(OpenCV_DIR "C:/Users/2-16/Downloads/opencv/build")
endif()
find_package(OpenCV REQUIRED)

set(COMMON_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/inc")
set(COMMON_SRCS
    src/RTSPClient.cpp
    src/XMLParser.cpp
)

add_executable(app
    src/main.cpp
    ${COMMON_SRCS}
)
target_include_directories(app PRIVATE ${COMMON_INCLUDE_DIR})
target_link_libraries(app PRIVATE ${OpenCV_LIBS})

add_executable(camera_client
    src/camera_client.cpp
    ${COMMON_SRCS}
)
target_include_directories(camera_client PRIVATE ${COMMON_INCLUDE_DIR})
target_link_libraries(camera_client PRIVATE ${OpenCV_LIBS})

add_executable(camera_RBF
    src/camera_RBF.cpp
    ${COMMON_SRCS}
)
target_include_directories(camera_RBF PRIVATE ${COMMON_INCLUDE_DIR})
target_link_libraries(camera_RBF PRIVATE ${OpenCV_LIBS})

if(WIN32)
    target_link_libraries(app PRIVATE ws2_32)
    target_link_libraries(camera_client PRIVATE ws2_32)
    target_link_libraries(camera_RBF PRIVATE ws2_32)
endif()

