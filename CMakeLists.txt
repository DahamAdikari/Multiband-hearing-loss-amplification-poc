cmake_minimum_required(VERSION 3.15)
project(cleartone_audio_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(cleartone_audio_engine SHARED 
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp 
)

message(STATUS "CLEAR_TONE_CMAKE: CMAKE_CURRENT_SOURCE_DIR is ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "CLEAR_TONE_CMAKE: Building library: cleartone_audio_engine")