# Building

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