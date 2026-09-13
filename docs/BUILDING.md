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

## Dependencies on Windows

- Visual Studio 2022 with the **Desktop development with C++** workload, or the
  standalone Build Tools. The compiler is MSVC; MinGW is not tested here.
- CMake 3.16 or newer, and Git with submodule support.
- Qt 6 for MSVC, from the official Qt installer. Note the path you install it
  to; CMake needs it.

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

## Windows build

Configure from a Developer Command Prompt, pointing CMake at Qt:

```bat
cmake -S . -B build ^
  -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/msvc2022_64"
cmake --build build --config Release --parallel
```

Everything lands in one directory, because Windows resolves a DLL from the
folder holding the executable and there is no RPATH to send it elsewhere:

```text
build\bin\Release\audio_to_text.exe       Qt interface
build\bin\Release\audio_to_text_cli.exe   console worker
build\bin\Release\whisper.dll             and the ggml libraries
```

Qt's own libraries are not there yet, so the interface will not start from the
build tree until either Qt's `bin` is on `PATH` or `windeployqt` has been run
against it. Installing does that for you:

```bat
cmake --install build --config Release --prefix C:\opt\audio-to-text
```

The install step invokes `windeployqt` on the installed executable, which
copies the Qt libraries and the platform plugin beside it. If `windeployqt`
cannot be found, CMake says so at configure time and the installed application
will not start.

To produce an archive:

```bat
cpack -G ZIP -C Release
```

### What is not there yet

- **No installer.** The ZIP is unpack-and-run. A Start Menu shortcut and a
  signed installer are separate work.
- **No model downloader.** `scripts/install-model.sh` is a shell script and is
  not installed on Windows. Fetch the model by hand into
  `%LOCALAPPDATA%\audio-to-text\models\` for now.
- **Unsigned binaries.** SmartScreen will warn on anything downloaded from the
  internet until the installer is code-signed.
- **The command-line test suite does not run.** See
  [TESTING.md](TESTING.md).

## Checks before pushing

Everything is checked locally; there is no hosted pipeline to wait on:

```bash
tests/run_gates.sh          # every gate
tests/run_gates.sh --quick  # skip the sanitizer build
```

That runs the guarantee and formatting checks, builds with
`-DAUDIO_TO_TEXT_WARNINGS_AS_ERRORS=ON`, runs the tests, and runs the
sanitizers. See [QUALITY_GATES.md](QUALITY_GATES.md).

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

On MSVC the equivalent is:

```text
/W4 /permissive- /utf-8
```

`/permissive-` matters most: it rejects the Microsoft extensions that let
non-portable code compile quietly, which is the failure mode this project
cares about.

Warnings from third-party Whisper code may still appear because that dependency
is built as part of the project. They are not changed by this project baseline.