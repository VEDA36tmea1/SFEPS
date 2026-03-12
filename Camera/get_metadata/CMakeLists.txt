cmake_minimum_required(VERSION 3.10)
project(CameraGetMetadata)

add_library(camera_metadata_parser STATIC
    src/XMLParser.cpp
)

target_include_directories(camera_metadata_parser
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
)
