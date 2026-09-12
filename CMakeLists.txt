cmake_minimum_required(VERSION 3.16)

project(AudioToText)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AUDIO_TO_TEXT_ENABLE_WARNINGS "Enable strict compiler warnings" ON)
option(AUDIO_TO_TEXT_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)

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

if(AUDIO_TO_TEXT_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(audio_to_text_cli PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_compile_options(audio_to_text PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

if(AUDIO_TO_TEXT_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(AUDIO_TO_TEXT_SANITIZER_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_compile_options(audio_to_text_cli PRIVATE ${AUDIO_TO_TEXT_SANITIZER_FLAGS})
    target_link_options(audio_to_text_cli PRIVATE ${AUDIO_TO_TEXT_SANITIZER_FLAGS})
endif()