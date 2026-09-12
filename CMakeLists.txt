cmake_minimum_required(VERSION 3.16)

project(AudioToText)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/whisper.cpp)

find_package(Threads REQUIRED)

add_executable(audio_to_text
    main.cpp
)

target_include_directories(audio_to_text PRIVATE
    third_party
)

target_link_libraries(audio_to_text PRIVATE
    whisper
    Threads::Threads
    ${CMAKE_DL_LIBS}
    m
)