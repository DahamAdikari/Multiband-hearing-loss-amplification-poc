cmake_minimum_required(VERSION 3.15)
project(cleartone_audio_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------------
# Main shared library (FFI / Android)
# -----------------------------
find_package(oboe REQUIRED CONFIG)
find_library(log-lib log)

add_library(cleartone_audio_engine SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/oboe_engine.cpp
)

target_link_libraries(cleartone_audio_engine PRIVATE oboe::oboe log)

# -----------------------------
# Desktop real-time executable
# -----------------------------
if (NOT ANDROID)
    find_package(portaudio CONFIG QUIET)
    if (NOT portaudio_FOUND)
        message(STATUS "CLEAR_TONE_CMAKE: portaudio config not found, trying pkg-config")
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)
        add_library(portaudio ALIAS PkgConfig::PORTAUDIO)
    endif()

    add_executable(realtime_multiband_stream
        ${CMAKE_CURRENT_SOURCE_DIR}/src/realtime_multiband_stream.cpp
    )

    target_link_libraries(realtime_multiband_stream PRIVATE portaudio)
    message(STATUS "CLEAR_TONE_CMAKE: Building executable: realtime_multiband_stream")
endif()

message(STATUS "CLEAR_TONE_CMAKE: CMAKE_CURRENT_SOURCE_DIR is ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "CLEAR_TONE_CMAKE: Building library: cleartone_audio_engine")