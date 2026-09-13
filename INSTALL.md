# Installing

Offline speech to text for Linux and Windows. Audio is captured, analysed and
transcribed on your own machine; nothing is uploaded. See
[docs/PRIVACY.md](docs/PRIVACY.md).

## From a package (Fedora)

```bash
sudo dnf install ./audio-to-text-*-Linux.rpm
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

## From a package (Windows)

Run `audio-to-text-<version>-win64.exe` and accept the default location. The
installer places the program, the Whisper libraries and the Qt libraries in one
directory, and adds **Audio to Text** to the Start Menu.

Windows will warn before it runs: the installer is not code-signed, so
SmartScreen shows *"Windows protected your PC"*. Choose **More info**, then
**Run anyway**. There is no way around this short of buying a code-signing
certificate, and you should be suspicious of any download that asks you to
bypass the warning -- verify the file came from this project's releases page.

The ZIP is the same files with no installer and no Start Menu entry. Unpack it
anywhere and run `bin\audio_to_text.exe`.

No model is included. Fetch one:

```powershell
& "$env:ProgramFiles\Audio to Text\bin\install-model.ps1" base.en
```

It downloads into `%LOCALAPPDATA%\audio-to-text\models` and prints the file's
SHA-256. Then check the installation:

```powershell
& "$env:ProgramFiles\Audio to Text\bin\audio_to_text_cli.exe" --version
& "$env:ProgramFiles\Audio to Text\bin\audio_to_text_cli.exe" --list-devices
```

If `--list-devices` shows nothing, or recording produces silence, check
**Settings -> Privacy & security -> Microphone** and confirm that *"Let desktop
apps access your microphone"* is on. Windows blocks the device without telling
the application why, so this looks like a broken microphone rather than a
permission.

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

x86-64 Linux or Windows, about 1 GB free memory for `base.en`, a microphone, and
no network connection at all after the model is in place. Full detail in
[docs/PERFORMANCE.md](docs/PERFORMANCE.md).

Windows support is built and packaged from the same source as the Linux build,
but has not yet been run on a Windows machine. Treat this release as untested
there and report what breaks.

## Where your files go

```text
~/.local/share/audio-to-text/recordings/     WAV audio
~/.local/share/audio-to-text/transcripts/    transcription text
~/.local/share/audio-to-text/models/         models installed with --user
```

On Windows the same three directories sit under
`%LOCALAPPDATA%\audio-to-text\`.

They are readable only by you: on Linux through explicit permissions, on
Windows through the per-user ACL that `%LOCALAPPDATA%` already carries. Delete
everything stored with:

```bash
audio_to_text_cli --delete-recordings
```

or the **Delete recordings** button in the interface.

## Uninstalling

Fedora:

```bash
sudo dnf remove audio-to-text
rm -rf ~/.local/share/audio-to-text
```

Windows: uninstall **Audio to Text** from *Settings -> Apps*, then remove your
data if you want it gone:

```powershell
Remove-Item -Recurse -Force "$env:LOCALAPPDATA\audio-to-text"
```

On both systems the uninstaller removes only the program. The second command
removes your recordings, transcripts and any model you installed -- which is
why it is separate, and deliberately not something an uninstaller decides for
you. Nothing is left elsewhere.
