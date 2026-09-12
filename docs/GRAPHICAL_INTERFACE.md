# Graphical interface

The Qt6 interface is built as `audio_to_text`. The existing recorder and Whisper pipeline remains in `audio_to_text_cli`; the GUI controls that worker asynchronously with `QProcess`.

## Build prerequisites

On Fedora, install Qt6 development files:

```bash
sudo dnf install qt6-qtbase-devel
```

Then configure and build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

## Run

From the project root:

```bash
./build/audio_to_text
```

The GUI provides:

- `base.en` and `small.en` model selection
- Start and Stop Recording controls
- recording duration
- recording and transcription status
- partial and final transcription output
- Save and Copy actions
- microphone and model error messages

Whisper runs in the CLI worker process, so the Qt event loop remains responsive. The worker uses streaming windows while recording and performs the final VAD/noise-reduction transcription pass after stopping.