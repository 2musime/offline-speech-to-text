#!/bin/bash
# Every quality gate, in one command. Run before pushing and before releasing.
#
# These gates deliberately do not depend on a hosted CI service: they run the
# same way on any machine that can build the project.
#
# Usage: tests/run_gates.sh [--quick]
#   --quick  skip the sanitizer build, which is the slow part
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

failed=""
step() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
record() { [ "$1" = "0" ] || failed="$failed\n  - $2"; }

step "Repository and privacy guarantees"
./tests/check_guarantees.sh
record $? "check_guarantees.sh"

step "Formatting"
./tests/check_style.sh
record $? "check_style.sh"

step "Build with warnings as errors"
cmake -S . -B build-release \
    -DCMAKE_BUILD_TYPE=Release \
    -DAUDIO_TO_TEXT_ENABLE_WARNINGS=ON \
    -DAUDIO_TO_TEXT_WARNINGS_AS_ERRORS=ON > /dev/null
cmake --build build-release --parallel "$(nproc)" 2>&1 \
    | grep -E "warning:|error:" | grep -v third_party | head -20
build_status=${PIPESTATUS[0]}
record "$build_status" "build with -Werror"
[ "$build_status" = "0" ] && echo "  no warnings from this project's targets"

step "Tests"
QT_QPA_PLATFORM=offscreen ctest --test-dir build-release --output-on-failure
record $? "ctest"

step "Binaries cannot reach the network"
./tests/check_guarantees.sh "$ROOT/build-release"
record $? "no-network check against built binaries"

if [ "$QUICK" = "0" ]; then
    step "Sanitizers"
    cmake -S . -B build-sanitize \
        -DCMAKE_BUILD_TYPE=Debug \
        -DAUDIO_TO_TEXT_ENABLE_SANITIZERS=ON > /dev/null
    cmake --build build-sanitize --parallel "$(nproc)" \
        --target audio_to_text_unit_tests audio_to_text_gui_tests > /dev/null 2>&1
    ASAN_OPTIONS=detect_leaks=1 ./build-sanitize/audio_to_text_unit_tests | tail -1
    record ${PIPESTATUS[0]} "unit tests under sanitizers"
    # Qt's own allocations at exit are not this project's leaks.
    QT_QPA_PLATFORM=offscreen ASAN_OPTIONS=detect_leaks=0 \
        ./build-sanitize/audio_to_text_gui_tests | tail -1
    record ${PIPESTATUS[0]} "interface tests under sanitizers"
else
    printf '\n(skipping sanitizers: --quick)\n'
fi

printf '\n'
if [ -z "$failed" ]; then
    printf '\033[1mAll gates passed.\033[0m\n'
    exit 0
fi
printf '\033[1mFAILED:\033[0m%b\n' "$failed"
exit 1
