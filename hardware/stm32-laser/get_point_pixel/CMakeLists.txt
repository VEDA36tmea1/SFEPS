cmake_minimum_required(VERSION 3.10)
project(get_point_pixel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)

add_executable(get_point_pixel
    get_point_pixel.cpp
)

target_include_directories(get_point_pixel PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(get_point_pixel PRIVATE ${OpenCV_LIBS})

