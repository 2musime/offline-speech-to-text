# Audio to Text

Speech to text that runs entirely on your own computer. Nothing is uploaded, and
the program opens no network connection.

Linux, x86-64. Qt 6 interface, [whisper.cpp](https://github.com/ggml-org/whisper.cpp)
for transcription, CPU only.

## Install

```bash
sudo dnf install ./audio-to-text-*-Linux.rpm
audio-to-text-install-model base.en --user
audio_to_text
```

Models are not packaged; the command above is the only step that uses the
network. See [INSTALL.md](INSTALL.md).

## Build

```bash
git clone --recurse-submodules git@github.com:2musime/offline-speech-to-text.git
cd offline-speech-to-text
sudo dnf install gcc-c++ cmake make qt6-qtbase-devel
cmake -S . -B build-release && cmake --build build-release --parallel
bash scripts/install-model.sh base.en --dir models
./build-release/audio_to_text
```

## Use

Three screens: **Home**, **Recording**, **Transcripts**.

Choose a model, microphone and time limit, record, and the transcript appears.
Partial text arrives while you speak; the full transcript is written when you
stop. Past transcripts are readable and deletable under **Transcripts**.

There is also a command-line worker:

```bash
./build-release/audio_to_text_cli models/ggml-base.en.bin --stream
./build-release/audio_to_text_cli --list-devices
./build-release/audio_to_text_cli --privacy
./build-release/audio_to_text_cli --delete-recordings
```

## Where your files go

```text
~/.local/share/audio-to-text/recordings/    WAV audio
~/.local/share/audio-to-text/transcripts/   transcription text
```

Directories are created `0700` and files `0600`, readable only by you. Nothing
is written to a log file.

## Privacy

Audio is captured, analysed and transcribed locally. Neither binary imports a
network syscall, and a full transcription completes inside a network namespace
with no interfaces. The checks are in `tests/check_guarantees.sh` and run on
every build. See [docs/PRIVACY.md](docs/PRIVACY.md).

## Performance

12 seconds of speech on a 12-thread machine:

| Model | Threads | Transcribe | Real-time factor |
|---|---:|---:|---:|
| `base.en` | 8 | 1.0 s | 0.08 |
| `small.en` | 8 | 3.5 s | 0.29 |

`base.en` is the default. Measure your own with
`audio_to_text_cli --benchmark --input speech.wav models/ggml-base.en.bin`.
See [docs/PERFORMANCE.md](docs/PERFORMANCE.md) for hardware requirements.

## Development

```bash
tests/run_gates.sh          # guarantees, style, warnings, tests, sanitizers
tests/run_gates.sh --quick  # skip the sanitizer build
```

| Topic | Document |
|---|---|
| Building and dependencies | [docs/BUILDING.md](docs/BUILDING.md) |
| Interface and keyboard | [docs/GRAPHICAL_INTERFACE.md](docs/GRAPHICAL_INTERFACE.md) |
| Tests | [docs/TESTING.md](docs/TESTING.md) |
| Quality gates | [docs/QUALITY_GATES.md](docs/QUALITY_GATES.md) |
| Where files go, and confinement | [docs/FILE_STORAGE.md](docs/FILE_STORAGE.md) |
| Models and checksums | [docs/MODEL_VALIDATION.md](docs/MODEL_VALIDATION.md) |
| Streaming behaviour | [docs/STREAMING_TRANSCRIPTION.md](docs/STREAMING_TRANSCRIPTION.md) |
| Releasing | [docs/RELEASE.md](docs/RELEASE.md) |

## Limitations

- Fedora and x86-64 only; no `.deb`, no Flatpak, no GPU backend
- English models (`base.en`, `small.en`); multilingual models load but are untested
- No audio playback; transcripts are text only
- No automated test drives the interface
