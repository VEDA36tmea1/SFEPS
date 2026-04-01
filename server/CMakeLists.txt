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

# OpenCV를 요구하는 Camera/get_metadata 툴 빌드는 기본적으로 끄고,
# 서버가 필요한 XML 파서 라이브러리만 항상 빌드한다.
option(SFEPS_BUILD_CAMERA_GET_METADATA_TOOLS "Build OpenCV-based Camera/get_metadata tools" OFF)

add_library(camera_metadata_parser STATIC
    ${CAMERA_GET_METADATA_DIR}/src/XMLParser.cpp
)
target_include_directories(camera_metadata_parser PUBLIC
    ${CAMERA_GET_METADATA_DIR}/inc
)

if(SFEPS_BUILD_CAMERA_GET_METADATA_TOOLS AND EXISTS "${CAMERA_GET_METADATA_DIR}/CMakeLists.txt")
    add_subdirectory(${CAMERA_GET_METADATA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/camera_get_metadata)
endif()

# 4. 실행 파일 (오디오 모듈 소스 포함)
add_executable(smart_server
    src/main.cpp
    src/core/utils/text_utils.cpp
    src/core/utils/env_utils.cpp
    src/core/config/security_runtime.cpp
    src/modules/streaming/rfid_image_pipeline.cpp
    src/core/ops/log.cpp
    src/modules/streaming/recorder.cpp
    src/modules/streaming/analytics.cpp
    src/core/ops/cleanup.cpp
    src/modules/auth/auth.cpp
    src/core/config/runtime_config.cpp
    src/core/network/net_utils.cpp
    src/core/bootstrap/app_services.cpp
    src/services/service_shared.cpp
    src/services/transport_utils.cpp
    src/modules/voice/audio_service.cpp
    src/modules/alerts/alert_service.cpp
    src/modules/ui/video_catalog_events.cpp
    src/modules/ui/video_catalog_service.cpp
    src/modules/ui/position_service.cpp
    src/modules/auth/auth_service.cpp
    src/modules/streaming/rfid_monitor.cpp
    src/modules/alerts/alert.cpp
    src/core/network/tls_server.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/services
    ${CAMERA_GET_METADATA_DIR}/inc
    ${AUDIO_SPEAKER_UNIT_DIR}/include
)

# 6. 라이브러리 링크 (ALSA 오디오 재생)
target_link_libraries(smart_server PRIVATE
    ${MARIADB_LIBRARIES}
    ${FFMPEG_LIBRARIES}
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    camera_metadata_parser
    asound
)
