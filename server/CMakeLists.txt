cmake_minimum_required(VERSION 3.10)
project(SFEPS_Server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. pkg-config 로드
find_package(PkgConfig REQUIRED)

# 2. 외부 라이브러리 탐색
pkg_check_modules(MARIADB REQUIRED mariadb)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# 3. Audio_Speaker_Unit 경로 (링 버퍼 + ALSA 재생)
set(AUDIO_SPEAKER_UNIT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../hardware/Raspi-driver/Audio_Speaker_Unit)
set(CAMERA_GET_METADATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../Camera/get_metadata)

if(EXISTS "${CAMERA_GET_METADATA_DIR}/CMakeLists.txt")
    add_subdirectory(${CAMERA_GET_METADATA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/camera_get_metadata)
else()
    add_library(camera_metadata_parser STATIC
        ${CAMERA_GET_METADATA_DIR}/src/XMLParser.cpp
    )
    target_include_directories(camera_metadata_parser PUBLIC
        ${CAMERA_GET_METADATA_DIR}/inc
    )
endif()

# 4. 실행 파일 (오디오 모듈 소스 포함)
add_executable(smart_server
    src/main.cpp
    src/log.cpp
    src/recorder.cpp
    src/analytics.cpp
    src/cleanup.cpp
    src/auth.cpp
    src/runtime_config.cpp
    src/net_utils.cpp
    src/app_services.cpp
    src/rfid_monitor.cpp
    # src/event_matcher.cpp  # disabled: legacy matcher module (not used in runtime path)
    src/alert.cpp
    src/esp_manager.cpp
    src/tls_server.cpp
    ${CAMERA_GET_METADATA_DIR}/src/XMLParser.cpp
    ${AUDIO_SPEAKER_UNIT_DIR}/src/audio_ring_buffer.cpp
    ${AUDIO_SPEAKER_UNIT_DIR}/src/audio_playback.cpp
)

set_target_properties(smart_server PROPERTIES OUTPUT_NAME "smart_server.bin")

add_custom_command(TARGET smart_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_CURRENT_BINARY_DIR}/smart_server
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_CURRENT_SOURCE_DIR}/run_server.sh
            ${CMAKE_CURRENT_BINARY_DIR}/smart_server
    COMMENT "Create launcher symlink build/smart_server -> run_server.sh"
)

# 5. 헤더 경로
target_include_directories(smart_server PRIVATE
    ${MARIADB_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CAMERA_GET_METADATA_DIR}/inc
    ${AUDIO_SPEAKER_UNIT_DIR}/include
    ${CAMERA_GET_METADATA_DIR}/inc
)

# 6. 라이브러리 링크 (ALSA 오디오 재생)
target_link_libraries(smart_server PRIVATE
    ${MARIADB_LIBRARIES}
    ${FFMPEG_LIBRARIES}
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    asound
)
