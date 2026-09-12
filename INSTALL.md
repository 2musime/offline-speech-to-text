# Installing

Offline speech to text for Linux. Audio is captured, analysed and transcribed on
your own machine; nothing is uploaded. See [docs/PRIVACY.md](docs/PRIVACY.md).

## From a package (Fedora)

```bash
sudo dnf install ./audio-to-text-1.0.0-Linux.rpm
```

The package pulls in `qt6-qtbase-gui`. It does **not** include a speech model:
models are hundreds of megabytes and downloading one is the only step that ever
touches the network.

Install a model after the application:

```bash
audio-to-text-install-model base.en --user
```

| Model | Size | Use |
|---|---|---|
| `base.en` | 142 MB | the default; fast, good accuracy |
| `small.en` | 466 MB | slower, better accuracy |
| `tiny.en` | 75 MB | fastest, least accurate |

Then check it works:

```bash
audio_to_text_cli --version
audio_to_text_cli --list-devices
audio_to_text          # the graphical interface
```

The GUI also appears in the desktop menu under Sound &amp; Video.

## From source

See [docs/BUILDING.md](docs/BUILDING.md). In short:

```bash
git clone --recurse-submodules git@github.com:2musime/offline-speech-to-text.git
cd offline-speech-to-text
sudo dnf install gcc-c++ cmake make qt6-qtbase-devel
cmake -S . -B build-release
cmake --build build-release --parallel
bash scripts/install-model.sh base.en --dir models
./build-release/audio_to_text
```

## Requirements

x86-64 Linux, about 1 GB free memory for `base.en`, a microphone, and no network
connection at all after the model is in place. Full detail in
[docs/PERFORMANCE.md](docs/PERFORMANCE.md).

## Where your files go

```text
~/.local/share/audio-to-text/recordings/     WAV audio
~/.local/share/audio-to-text/transcripts/    transcription text
~/.local/share/audio-to-text/models/         models installed with --user
```

Directories are created readable only by you. Delete everything stored with:

```bash
audio_to_text_cli --delete-recordings
```

or the **Delete recordings** button in the interface.

## Uninstalling

```bash
sudo dnf remove audio-to-text
rm -rf ~/.local/share/audio-to-text
```

The second command removes your recordings, transcripts and any model installed
with `--user`. Nothing is left elsewhere.
