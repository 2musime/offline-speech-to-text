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
find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(audio_to_text_cli
    main.cpp
)

target_include_directories(audio_to_text_cli PRIVATE
    third_party
)

target_link_libraries(audio_to_text_cli PRIVATE
    whisper
    Threads::Threads
    ${CMAKE_DL_LIBS}
    m
)

add_executable(audio_to_text
    gui_main.cpp
)

target_link_libraries(audio_to_text PRIVATE
    Qt6::Widgets
)