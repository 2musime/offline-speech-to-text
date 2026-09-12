# Release

## Versioning

The version lives in one place, `project(... VERSION x.y.z)` in `CMakeLists.txt`.
CMake generates `version.h` from it and records the short git commit, so a build
can always identify itself:

```bash
audio_to_text_cli --version
# audio_to_text 1.0.0 (e077594, Release)
```

The GUI shows the same under **About**, and in its title bar.

## Checklist

Work top to bottom. Anything that fails stops the release.

**1. The tree is clean**

```bash
git status --porcelain          # empty
git log --oneline develop..HEAD # nothing unmerged
```

**2. Every gate passes**

```bash
tests/run_gates.sh
```

Guarantees, formatting, a build with warnings as errors, the full test suite,
and the sanitizers. It names whatever failed. See
[QUALITY_GATES.md](QUALITY_GATES.md).

**3. The interface has been used by a person**

Start a recording, speak, stop, and confirm the transcript. No automated test
clicks a button, so this step is the only thing standing between a broken
interface and a release.

**4. The version is bumped** in `CMakeLists.txt`, and `--version` reports it.

**5. A fresh clone builds**

```bash
git clone --recurse-submodules <url> /tmp/release-check
cmake -S /tmp/release-check -B /tmp/release-check/build
cmake --build /tmp/release-check/build --parallel
```

**6. Packages build**

```bash
cmake --build build-release --target package
```

Produces `audio-to-text-<version>-Linux.rpm` and a `.tar.gz`.

**7. The package installs on a clean system** with no build tools present, and
the installed binaries run. See below.

**8. Performance has not regressed**

```bash
./build-release/audio_to_text_cli --benchmark --input speech.wav \
  models/ggml-base.en.bin models/ggml-small.en.bin
```

Compare against [PERFORMANCE.md](PERFORMANCE.md). Run-to-run variance is about
10%; do not chase differences smaller than that.

**9. Tag and publish**

```bash
git tag -a v1.0.0 -m "1.0.0"
git push origin v1.0.0
```

## Building packages

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel
cmake --build build-release --target package
```

`rpm-build` must be installed for the RPM generator.

## What ships, and what does not

| Component | Packaged | Why |
|---|---|---|
| `audio_to_text`, `audio_to_text_cli` | yes | the application |
| `libwhisper`, `libggml`, `libggml-base`, `libggml-cpu` | yes, privately | built from the pinned submodule; installed to `/usr/lib64/audio-to-text` rather than competing with a system Whisper |
| Qt 6 | no, required | comes from the distribution as `qt6-qtbase-gui` |
| Desktop entry | yes | so the GUI appears in the menu |
| `audio-to-text-install-model` | yes | fetches a model after installation |
| Whisper models | **no** | hundreds of megabytes, and downloading is the one networked step |

Models are deliberately excluded. Packaging one would make the download implicit
and inflate the package by an order of magnitude.

## Verifying a clean installation

This step is not optional, and it cannot be done on a development machine.

A machine that built the software already has every library the build produced,
so an installed binary can resolve them by accident even when the package is
wrong. The first package built here looked correct, listed the right files, and
declared the right dependency, but the installed CLI would not start:

```text
audio_to_text_cli: error while loading shared libraries: libwhisper.so.1
```

The runtime path had been built from `CMAKE_INSTALL_PREFIX`, which is
`/usr/local` by default, while CPack stages the package under `/usr`. The binary
searched a directory that did not exist. Only a machine without the build tree
exposes that. The path is now `$ORIGIN`-relative, so it resolves against the
binary's own location instead of a prefix fixed at configure time.

Run it in a throwaway container, which needs no spare machine:

```bash
podman run --rm -v "$PWD/build-release:/pkg:Z" fedora:41 bash -c '
  dnf -y install /pkg/audio-to-text-*.rpm &&
  audio_to_text_cli --version &&
  ldd /usr/bin/audio_to_text_cli | grep "not found" && echo BROKEN || echo OK'
```

Confirm before shipping:

- no library reports `not found` for either binary
- `audio_to_text_cli --version` prints the expected version
- the GUI starts under `QT_QPA_PLATFORM=offscreen`
- `desktop-file-validate` accepts the desktop entry
- `audio-to-text-install-model --help` runs



```bash
sudo dnf install ./audio-to-text-1.0.0-Linux.rpm
audio_to_text_cli --version
audio-to-text-install-model base.en --user
audio_to_text_cli --list-devices
```

The installed binaries carry an RPATH pointing at `/usr/lib64/audio-to-text`, so
they resolve the private Whisper libraries without `LD_LIBRARY_PATH`.
