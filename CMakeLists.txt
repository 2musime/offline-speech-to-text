cmake_minimum_required(VERSION 3.16)

project(AudioToText
    VERSION 1.1.0
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

# libm is a separate library only on Unix; the Microsoft CRT already has it, and
# CMAKE_DL_LIBS is empty on Windows. Naming "m" unconditionally fails the link.
set(AUDIO_TO_TEXT_PLATFORM_LIBS ${CMAKE_DL_LIBS})
if(NOT WIN32)
    list(APPEND AUDIO_TO_TEXT_PLATFORM_LIBS m)
endif()

# Windows resolves a DLL from the directory holding the executable, and there is
# no RPATH to point anywhere else. ggml already sends its shared libraries to
# <build>/bin, so the executables have to go there too or the build tree is not
# runnable. Left alone elsewhere: Linux has the RPATH set further down, and
# moving the binaries would break every script and document that names them.
if(WIN32)
    set(AUDIO_TO_TEXT_RUNTIME_DIR "${CMAKE_BINARY_DIR}/bin")
endif()

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
    ${AUDIO_TO_TEXT_PLATFORM_LIBS}
)

add_executable(audio_to_text
    gui_main.cpp
    miniaudio_impl.cpp
)

if(WIN32)
    # Compiled into the executable, which is where Explorer, the taskbar and
    # Alt-Tab look for an icon. Nothing at run time reads it.
    target_sources(audio_to_text PRIVATE packaging/audio-to-text.rc)
endif()

# The window's own icon, compiled in rather than loaded from disk so it is
# present in the build tree and after an install to any prefix. A PNG rather
# than the .ico: PNG decoding is built into Qt, while ICO arrives as a plugin
# that may not be deployed, and a missing plugin fails silently.
qt_add_resources(audio_to_text "audio_to_text_icon"
    PREFIX "/icons"
    BASE packaging/icons
    FILES packaging/icons/audio-to-text-512.png)

target_include_directories(audio_to_text PRIVATE
    .
    third_party
    "${CMAKE_BINARY_DIR}/generated"
)

target_link_libraries(audio_to_text PRIVATE
    Qt6::Widgets
    Threads::Threads
    ${AUDIO_TO_TEXT_PLATFORM_LIBS}
)

if(WIN32)
    # WIN32_EXECUTABLE picks the GUI subsystem, without which Windows opens a
    # console window behind the interface. Qt supplies the entry point for it.
    # The worker stays a console program: Qt starts it with CREATE_NO_WINDOW
    # for every channel mode except ForwardedChannels, and this GUI merges its
    # channels, so no window should appear. Confirm that on a real machine.
    set_target_properties(audio_to_text PROPERTIES WIN32_EXECUTABLE TRUE)
    set_target_properties(audio_to_text audio_to_text_cli PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${AUDIO_TO_TEXT_RUNTIME_DIR}")
else()
    # So an installed binary finds the private Whisper and ggml libraries.
    set_target_properties(audio_to_text audio_to_text_cli PROPERTIES
        INSTALL_RPATH "${AUDIO_TO_TEXT_PRIVATE_LIBDIR}"
        BUILD_WITH_INSTALL_RPATH FALSE
        # Without this the installed binary keeps absolute build-tree paths,
        # which would resolve on this machine and nowhere else.
        INSTALL_RPATH_USE_LINK_PATH FALSE)
endif()

# MSVC needs its own spelling of all of this. Without the branch below a
# Windows build compiled with no warnings enabled at all, which is the one
# configuration where the portability code most needs checking.
if(AUDIO_TO_TEXT_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(AUDIO_TO_TEXT_WARNING_FLAGS -Wall -Wextra -Wpedantic)
    if(AUDIO_TO_TEXT_WARNINGS_AS_ERRORS)
        list(APPEND AUDIO_TO_TEXT_WARNING_FLAGS -Werror)
    endif()
elseif(AUDIO_TO_TEXT_ENABLE_WARNINGS AND MSVC)
    # /permissive- rejects the Microsoft extensions that let non-portable code
    # compile quietly; /utf-8 keeps source and literals in UTF-8 whatever the
    # machine's code page says.
    set(AUDIO_TO_TEXT_WARNING_FLAGS /W4 /permissive- /utf-8)
    if(AUDIO_TO_TEXT_WARNINGS_AS_ERRORS)
        list(APPEND AUDIO_TO_TEXT_WARNING_FLAGS /WX)
    endif()
endif()

if(AUDIO_TO_TEXT_WARNING_FLAGS)
    target_compile_options(audio_to_text_cli PRIVATE ${AUDIO_TO_TEXT_WARNING_FLAGS})
    target_compile_options(audio_to_text PRIVATE ${AUDIO_TO_TEXT_WARNING_FLAGS})
endif()

if(MSVC)
    # _wgetenv and localtime carry a deprecation warning on this compiler. The
    # replacements Microsoft suggests are not portable, and both calls are
    # already used safely, so the warning is turned off rather than the code
    # being contorted around it.
    target_compile_definitions(audio_to_text_cli PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(audio_to_text PRIVATE _CRT_SECURE_NO_WARNINGS)
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

if(WIN32)
    # The unit tests link Whisper, so they need the same directory as the DLLs.
    set_target_properties(audio_to_text_unit_tests audio_to_text_gui_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${AUDIO_TO_TEXT_RUNTIME_DIR}")
endif()

add_test(NAME unit COMMAND audio_to_text_unit_tests)
add_test(NAME gui COMMAND audio_to_text_gui_tests)
# The command-line suite is a shell script. Registering it on Windows would fail
# at run time rather than reporting honestly, so it is left out until it is
# ported; see docs/TESTING.md.
if(NOT WIN32)
    add_test(NAME cli COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_tests.sh $<TARGET_FILE:audio_to_text_cli>)
endif()

if(AUDIO_TO_TEXT_WARNING_FLAGS)
    target_compile_options(audio_to_text_unit_tests PRIVATE ${AUDIO_TO_TEXT_WARNING_FLAGS})
    target_compile_options(audio_to_text_gui_tests PRIVATE ${AUDIO_TO_TEXT_WARNING_FLAGS})
endif()

if(MSVC)
    target_compile_definitions(audio_to_text_unit_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(audio_to_text_gui_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
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
# privately rather than into the system library path. On Windows a DLL is only
# found beside the executable, so "privately" there means the same directory.
if(WIN32)
    install(TARGETS whisper ggml ggml-base ggml-cpu
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
    install(TARGETS whisper ggml ggml-base ggml-cpu
        LIBRARY DESTINATION lib64/audio-to-text
        RUNTIME DESTINATION lib64/audio-to-text)
endif()

# A desktop entry means nothing on Windows, where the Start Menu shortcut comes
# from the installer instead. The model downloader exists for both, in the
# language each platform can run without installing anything first.
if(WIN32)
    install(FILES scripts/install-model.ps1
        DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
    install(FILES packaging/audio-to-text.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

    # Named audio-to-text to match Icon= in the desktop entry. The sizes are
    # separate files rather than one scaled image because a desktop environment
    # picks the exact size it needs, and a 512 pixel glyph scaled to 16 loses
    # the thin strokes.
    install(DIRECTORY packaging/icons/hicolor
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons)

    install(PROGRAMS scripts/install-model.sh
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        RENAME audio-to-text-install-model)
endif()

# Qt's own libraries and its platform plugin are not covered by install(TARGETS)
# and have no system package to come from on Windows. windeployqt reads the
# installed executable and copies everything it needs next to it.
if(WIN32)
    # Qt does not export windeployqt as a target in every version, so it is
    # located next to qmake, which is exported.
    if(TARGET Qt6::qmake)
        get_target_property(AUDIO_TO_TEXT_QMAKE Qt6::qmake IMPORTED_LOCATION)
        get_filename_component(AUDIO_TO_TEXT_QT_BIN "${AUDIO_TO_TEXT_QMAKE}" DIRECTORY)
    endif()
    find_program(AUDIO_TO_TEXT_WINDEPLOYQT windeployqt HINTS "${AUDIO_TO_TEXT_QT_BIN}")

    if(AUDIO_TO_TEXT_WINDEPLOYQT)
        # COMMAND_ERROR_IS_FATAL would be shorter but arrived in CMake 3.19,
        # and this project supports 3.16.
        install(CODE "
            execute_process(
                COMMAND \"${AUDIO_TO_TEXT_WINDEPLOYQT}\"
                    --no-translations --no-system-d3d-compiler --no-opengl-sw
                    \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/audio_to_text.exe\"
                RESULT_VARIABLE deploy_result)
            if(NOT deploy_result EQUAL 0)
                message(FATAL_ERROR \"windeployqt failed: \${deploy_result}\")
            endif()")
    else()
        message(WARNING
            "windeployqt was not found. The installed application will not start "
            "until Qt's libraries are placed beside audio_to_text.exe.")
    endif()
endif()

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
if(WIN32)
    # The ZIP is unpack-and-run and always produced. NSIS is added only when
    # makensis is actually present, so a machine without it still gets an
    # archive rather than a failed cpack run.
    set(CPACK_GENERATOR "ZIP")
    find_program(AUDIO_TO_TEXT_MAKENSIS makensis)
    if(AUDIO_TO_TEXT_MAKENSIS)
        list(APPEND CPACK_GENERATOR "NSIS")
    else()
        message(STATUS
            "makensis was not found: cpack will produce the ZIP but no installer.")
    endif()

    # Named without the version, so an upgrade replaces the previous install
    # instead of leaving two copies side by side.
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Audio to Text")
    # Start Menu shortcut: the executable without its extension, then the label.
    # CPack looks for it under bin/, which is where it is installed.
    set(CPACK_PACKAGE_EXECUTABLES "audio_to_text" "Audio to Text")
    set(CPACK_NSIS_PACKAGE_NAME "Audio to Text")
    set(CPACK_NSIS_DISPLAY_NAME "Audio to Text ${PROJECT_VERSION}")
    set(CPACK_NSIS_URL_INFO_ABOUT "${PROJECT_HOMEPAGE_URL}")
    set(CPACK_NSIS_HELP_LINK "${PROJECT_HOMEPAGE_URL}")
    set(CPACK_NSIS_CONTACT "${CPACK_PACKAGE_CONTACT}")
    # Nothing here belongs on PATH, and an upgrade should not stack on top of
    # the files windeployqt left behind last time.
    set(CPACK_NSIS_MODIFY_PATH OFF)
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/audio-to-text.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/audio-to-text.ico")
    # What Add/Remove Programs shows beside the entry.
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\audio_to_text.exe")
    # Recordings and transcripts live in %LOCALAPPDATA% and are the user's, not
    # the installer's. Uninstalling removes the program and leaves them alone.
else()
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_GENERATOR "RPM;TGZ")

    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
    # Qt comes from the distribution; the model is downloaded separately and is
    # deliberately not packaged, being hundreds of megabytes.
    set(CPACK_RPM_PACKAGE_REQUIRES "qt6-qtbase-gui")
    set(CPACK_RPM_PACKAGE_AUTOREQPROV "no")
    # These directories belong to other packages; this one only adds files to
    # them. hicolor-icon-theme owns the icon tree.
    set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
        "/usr/lib64" "/usr/share/applications" "/usr/share/doc"
        "/usr/share/icons" "/usr/share/icons/hicolor")
endif()

include(CPack)
