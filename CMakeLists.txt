cmake_minimum_required(VERSION 3.16)

project(AudioToText
    VERSION 1.0.1
    DESCRIPTION "Offline speech to text"
    HOMEPAGE_URL "https://github.com/2musime/offline-speech-to-text"
    LANGUAGES CXX)

# Recorded in the binary so a bug report can name the exact build.
find_package(Git QUIET)
set(AUDIO_TO_TEXT_GIT_COMMIT "unknown")
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE AUDIO_TO_TEXT_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
configure_file(version.h.in "${CMAKE_BINARY_DIR}/generated/version.h" @ONLY)

# Our Whisper and ggml builds are pinned to this project, so they live in a
# private directory rather than competing with a system-wide install.
#
# The runtime path is relative to the binary, not absolute. An absolute path
# built from CMAKE_INSTALL_PREFIX is wrong whenever the package is installed
# somewhere else: CPack stages to /usr while the configure-time prefix was
# /usr/local, so the installed binary looked in a directory that did not exist
# and failed to start. $ORIGIN resolves against the binary's own location, so
# bin/ and lib64/ only have to stay siblings.
set(AUDIO_TO_TEXT_PRIVATE_LIBDIR "$ORIGIN/../lib64/audio-to-text")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AUDIO_TO_TEXT_ENABLE_WARNINGS "Enable strict compiler warnings" ON)
option(AUDIO_TO_TEXT_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)
# Applied only to this project's targets; vendored Whisper is not held to it.
option(AUDIO_TO_TEXT_WARNINGS_AS_ERRORS "Treat this project's warnings as errors" OFF)

set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
# EXCLUDE_FROM_ALL keeps the vendored project's own install rules out of our
# package. Without it the RPM also shipped ggml headers into /usr/include,
# CMake and pkg-config files, a second copy of every library in /usr/lib64, and
# libparakeet, which this application does not use. The libraries we do need are
# installed explicitly, privately, further down.
add_subdirectory(third_party/whisper.cpp EXCLUDE_FROM_ALL)

find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(audio_to_text_cli
    main.cpp
)

target_include_directories(audio_to_text_cli PRIVATE
    third_party
    "${CMAKE_BINARY_DIR}/generated"
)

target_link_libraries(audio_to_text_cli PRIVATE
    whisper
    Threads::Threads
    ${CMAKE_DL_LIBS}
    m
)

add_executable(audio_to_text
    gui_main.cpp
    miniaudio_impl.cpp
)

target_include_directories(audio_to_text PRIVATE
    .
    third_party
    "${CMAKE_BINARY_DIR}/generated"
)

target_link_libraries(audio_to_text PRIVATE
    Qt6::Widgets
    Threads::Threads
    ${CMAKE_DL_LIBS}
    m
)

# So an installed binary finds the private Whisper and ggml libraries.
set_target_properties(audio_to_text audio_to_text_cli PROPERTIES
    INSTALL_RPATH "${AUDIO_TO_TEXT_PRIVATE_LIBDIR}"
    BUILD_WITH_INSTALL_RPATH FALSE
    # Without this the installed binary keeps absolute build-tree paths, which
    # would resolve on this machine and nowhere else.
    INSTALL_RPATH_USE_LINK_PATH FALSE)

if(AUDIO_TO_TEXT_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(AUDIO_TO_TEXT_WARNING_FLAGS -Wall -Wextra -Wpedantic)
    if(AUDIO_TO_TEXT_WARNINGS_AS_ERRORS)
        list(APPEND AUDIO_TO_TEXT_WARNING_FLAGS -Werror)
    endif()
    target_compile_options(audio_to_text_cli PRIVATE ${AUDIO_TO_TEXT_WARNING_FLAGS})
    target_compile_options(audio_to_text PRIVATE ${AUDIO_TO_TEXT_WARNING_FLAGS})
endif()

if(AUDIO_TO_TEXT_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(AUDIO_TO_TEXT_SANITIZER_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_compile_options(audio_to_text_cli PRIVATE ${AUDIO_TO_TEXT_SANITIZER_FLAGS})
    target_link_options(audio_to_text_cli PRIVATE ${AUDIO_TO_TEXT_SANITIZER_FLAGS})
endif()

enable_testing()

add_executable(audio_to_text_unit_tests tests/unit_tests.cpp)
target_include_directories(audio_to_text_unit_tests PRIVATE . tests third_party)
target_link_libraries(audio_to_text_unit_tests PRIVATE whisper Threads::Threads)

add_executable(audio_to_text_gui_tests tests/gui_tests.cpp)
target_include_directories(audio_to_text_gui_tests PRIVATE . tests)
target_link_libraries(audio_to_text_gui_tests PRIVATE Qt6::Core)

add_test(NAME unit COMMAND audio_to_text_unit_tests)
add_test(NAME gui COMMAND audio_to_text_gui_tests)
add_test(NAME cli COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_tests.sh $<TARGET_FILE:audio_to_text_cli>)

if(AUDIO_TO_TEXT_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(audio_to_text_unit_tests PRIVATE ${AUDIO_TO_TEXT_WARNING_FLAGS})
    target_compile_options(audio_to_text_gui_tests PRIVATE ${AUDIO_TO_TEXT_WARNING_FLAGS})
endif()

if(AUDIO_TO_TEXT_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(audio_to_text_unit_tests PRIVATE ${AUDIO_TO_TEXT_SANITIZER_FLAGS})
    target_link_options(audio_to_text_unit_tests PRIVATE ${AUDIO_TO_TEXT_SANITIZER_FLAGS})
    target_compile_options(audio_to_text_gui_tests PRIVATE ${AUDIO_TO_TEXT_SANITIZER_FLAGS})
    target_link_options(audio_to_text_gui_tests PRIVATE ${AUDIO_TO_TEXT_SANITIZER_FLAGS})
endif()

# ----------------------------------------------------------------- installation
include(GNUInstallDirs)

install(TARGETS audio_to_text audio_to_text_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Whisper and ggml are built from the pinned submodule, so they are installed
# privately rather than into the system library path.
install(TARGETS whisper ggml ggml-base ggml-cpu
    LIBRARY DESTINATION lib64/audio-to-text
    RUNTIME DESTINATION lib64/audio-to-text)

install(FILES packaging/audio-to-text.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

install(PROGRAMS scripts/install-model.sh
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    RENAME audio-to-text-install-model)

install(FILES
        docs/PRIVACY.md docs/BUILDING.md docs/MODEL_VALIDATION.md
        docs/PERFORMANCE.md docs/RELEASE.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR})

# --------------------------------------------------------------------- packaging
set(CPACK_PACKAGE_NAME "audio-to-text")
set(CPACK_PACKAGE_VENDOR "2musime")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_CONTACT "david@sokrab.com")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_GENERATOR "RPM;TGZ")

set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
# Qt comes from the distribution; the model is downloaded separately and is
# deliberately not packaged, being hundreds of megabytes.
set(CPACK_RPM_PACKAGE_REQUIRES "qt6-qtbase-gui")
set(CPACK_RPM_PACKAGE_AUTOREQPROV "no")
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    "/usr/lib64" "/usr/share/applications" "/usr/share/doc")

include(CPack)
