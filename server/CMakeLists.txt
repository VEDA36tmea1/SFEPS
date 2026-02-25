cmake_minimum_required(VERSION 3.10)
project(SFEPS_Server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. pkg-config 로드
find_package(PkgConfig REQUIRED)

# 2. 외부 라이브러리 탐색
pkg_check_modules(MARIADB REQUIRED mariadb)
pkg_check_modules(TINYXML2 REQUIRED tinyxml2)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale)
find_package(Threads REQUIRED)

# 3. Audio_Speaker_Unit 경로 (링 버퍼 + ALSA 재생)
set(AUDIO_SPEAKER_UNIT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../hardware/Raspi-driver/Audio_Speaker_Unit)

# 4. 실행 파일 (오디오 모듈 소스 포함)
add_executable(smart_server
    src/main.cpp
    src/log.cpp
    src/recorder.cpp
    src/analytics.cpp
    src/cleanup.cpp
    src/auth.cpp
    src/runtime_config.cpp
    src/rfid_monitor.cpp
    src/event_matcher.cpp
    src/alert.cpp
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
    ${TINYXML2_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AUDIO_SPEAKER_UNIT_DIR}/include
)

# 6. 라이브러리 링크 (ALSA 오디오 재생)
target_link_libraries(smart_server PRIVATE
    ${MARIADB_LIBRARIES}
    ${TINYXML2_LIBRARIES}
    ${FFMPEG_LIBRARIES}
    Threads::Threads
    asound
)
