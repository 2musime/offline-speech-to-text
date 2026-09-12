# Building

## Getting the source

Whisper is a submodule, so clone with it:

```bash
git clone --recurse-submodules git@github.com:2musime/offline-speech-to-text.git
```

In an existing checkout that predates the submodule being registered:

```bash
git submodule update --init --recursive
```

Without this, `third_party/whisper.cpp` is empty and CMake fails at
`add_subdirectory`.

## Dependencies

On Fedora, install the compiler, CMake, and Qt6 development package:

```bash
sudo dnf install gcc-c++ cmake make qt6-qtbase-devel
```

The Whisper source and miniaudio header are kept in `third_party/`.

## Production build

From the project root, use a fresh build directory:

```bash
cmake -S . -B build-release
cmake --build build-release --parallel
```

When no build type is supplied, CMake configures this project as `Release`.
The build produces:

```text
build-release/audio_to_text       Qt GUI
build-release/audio_to_text_cli  console worker
```

Run the GUI:

```bash
./build-release/audio_to_text
```

Run the CLI worker directly:

```bash
./build-release/audio_to_text_cli models/ggml-small.en.bin --threads 4
```

Recordings are limited to 15 seconds by default. Supported limits are 15, 45
and 60 seconds, and 300 or 600 seconds for dictation:

```bash
./build-release/audio_to_text_cli models/ggml-small.en.bin \
  --duration 45 --threads 4
```

The limit is enforced for both normal and streaming capture. Empty recordings,
unsupported duration values, buffer overflow, and truncated recordings produce
clear errors instead of unbounded memory growth.

## Checks before pushing

Everything the continuous integration workflow runs is available locally:

```bash
./tests/check_guarantees.sh        # nothing private tracked, no networking
./tests/check_style.sh             # whitespace, line endings, line length
ctest --test-dir build-release --output-on-failure
```

Add `-DAUDIO_TO_TEXT_WARNINGS_AS_ERRORS=ON` when configuring to match the
compiler settings used in the workflow. See [CI.md](CI.md).

## Sanitizer build

Use sanitizers during development. This build is for testing, not production
performance:

```bash
cmake -S . -B build-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DAUDIO_TO_TEXT_ENABLE_SANITIZERS=ON
cmake --build build-sanitize --parallel
```

Run the CLI worker under AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
ASAN_OPTIONS=detect_leaks=1 ./build-sanitize/audio_to_text_cli \
  models/ggml-small.en.bin --threads 2
```

## Compiler warnings

Project targets use:

```text
-Wall -Wextra -Wpedantic
```

Warnings from third-party Whisper code may still appear because that dependency
is built as part of the project. They are not changed by this project baseline.