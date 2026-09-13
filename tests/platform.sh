#!/bin/bash
# Platform facts, worked out once and shared by every gate script.
#
# The gates are one set of scripts, not one set per platform. On Windows they
# run under Git Bash, which ships with Git for Windows and is therefore already
# present on any machine that can clone this repository with its submodule. A
# second PowerShell implementation would be a second thing to keep correct, and
# the two would drift the moment one of them was edited in a hurry.
#
# What this file does not do is pretend. Some checks cannot run everywhere --
# there is no Windows equivalent of a network namespace -- and those are
# reported as skipped, never as passed. A gate that quietly does nothing is
# worse than a gate that is absent, because it looks like coverage.

# AUDIO_TO_TEXT_PLATFORM pins the answer instead of detecting it. It exists so
# one platform's branches can be exercised from another, which is the only way
# to know the Windows paths in these scripts were ever executed. Nothing in
# normal use sets it.
if [ -n "${AUDIO_TO_TEXT_PLATFORM:-}" ]; then
    PLATFORM="$AUDIO_TO_TEXT_PLATFORM"
else
    case "$(uname -s 2>/dev/null)" in
        Linux)                PLATFORM=linux ;;
        MINGW*|MSYS*|CYGWIN*) PLATFORM=windows ;;
        Darwin)               PLATFORM=macos ;;
        *)                    PLATFORM=unknown ;;
    esac
fi

if [ "$PLATFORM" = "windows" ]; then
    EXE=".exe"
else
    EXE=""
fi

# nproc is a GNU coreutils tool and is not in Git Bash.
cpu_count() {
    if command -v nproc > /dev/null 2>&1; then
        nproc
    elif [ -n "${NUMBER_OF_PROCESSORS:-}" ]; then
        echo "$NUMBER_OF_PROCESSORS"
    else
        echo 2
    fi
}

# Visual Studio chooses the configuration at build time rather than at configure
# time, so cmake and ctest both need telling which one to use. Ninja and Unix
# Makefiles decide at configure time and reject the argument.
if [ "$PLATFORM" = "windows" ]; then
    BUILD_CONFIG="--config Release"
    TEST_CONFIG="-C Release"
else
    BUILD_CONFIG=""
    TEST_CONFIG=""
fi

# Where the executables actually landed. On Windows they share a directory with
# the Whisper DLLs, and a multi-configuration generator adds a further level.
# Probing beats predicting: it is correct for Visual Studio and for Ninja.
binary_dir() { # build-directory
    if [ -f "$1/bin/Release/audio_to_text_cli$EXE" ]; then
        echo "$1/bin/Release"
    elif [ -f "$1/bin/audio_to_text_cli$EXE" ]; then
        echo "$1/bin"
    else
        echo "$1"
    fi
}
