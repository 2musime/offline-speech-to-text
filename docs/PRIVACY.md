# Privacy

Audio is captured, analysed and transcribed entirely on this computer. Whisper
runs locally against a model file on disk. No audio, transcript or metadata is
uploaded.

Read the notice from either interface:

```bash
./build-release/audio_to_text_cli --privacy
```

In the GUI, the **Privacy** button shows the same information together with the
directory currently in use.

## No network access

This is checked, not asserted.

**Nothing network-shaped is linked.** Neither binary imports a network syscall
(`socket`, `connect`, `bind`, `send`, `recv`, `getaddrinfo` and friends), and
the worker links no TLS, DNS or HTTP library:

```bash
nm -D --undefined-only build-release/audio_to_text_cli | awk '{print $2}' \
  | grep -vE '^_Z' | grep -xE "socket|connect|bind|send|recv|getaddrinfo"
```

Note when reading that output yourself: a plain search for `connect` also
matches Qt's `QObject::connectImpl` and `connectNotify`, which are signal/slot
machinery and have nothing to do with sockets. Excluding mangled C++ names
(`^_Z`) is what makes the check meaningful.

**It runs with no network at all.** A full transcription completes inside a
network namespace whose only interface is a downed loopback:

```bash
unshare -rn -- ./build-release/audio_to_text_cli \
  --benchmark --input speech.wav models/ggml-base.en.bin
```

The GUI also starts and runs normally in that namespace.

Models are the one thing that ever comes from the network, and only when you
download them yourself with the vendored script. See
[MODEL_VALIDATION.md](MODEL_VALIDATION.md).

## What is written, and where

Only inside the application data directory, and only readable by you — on
Linux through explicit permissions (directories `0700`, files `0600`), on
Windows through the per-user ACL that `%LOCALAPPDATA%` already carries. See
[FILE_STORAGE.md](FILE_STORAGE.md).

```text
<data>/recordings/     WAV audio
<data>/transcripts/    transcription text
```

The path is printed at startup, shown in the GUI's **Saving to** line, and
reported as `DATADIR|<path>`. It is never hidden from you.

## Logging

The application writes no log file. Nothing appends audio or transcribed text to
any file other than the transcript you asked for.

One honest caveat: transcribed text is printed to standard output, because that
is how the GUI receives it from the worker. Redirecting that output to a file is
the one way this program's text ends up somewhere you did not choose. Use
`--no-retain-transcript` if you want the text displayed and not stored at all.

Whisper's own diagnostics go to standard error and contain model geometry and
timings, never audio or transcribed words.

## Controls

| Control | CLI | GUI |
|---|---|---|
| Show the privacy notice | `--privacy` | **Privacy** button |
| Transcribe without keeping audio | `--no-retain-audio` | **Keep audio files** checkbox |
| Transcribe without saving text | `--no-retain-transcript` | not exposed |
| Delete everything stored | `--delete-recordings` | **Delete recordings** button |

Turning off audio retention still produces a transcript; only the WAV files are
skipped. Completion is keyed off the transcription itself rather than off a file
appearing, so both interfaces behave identically with retention off.

`--delete-recordings` removes regular files from the recordings and transcripts
directories. It is confined to the data directory and refuses to delete through
a symbolic link, reporting one instead:

```text
WARN|FILE_SAVING|Skipping a symbolic link rather than deleting through it: ...
DELETED|61|13795819
```

The GUI asks for confirmation first, and the button is disabled while a
recording or transcription is running so deletion cannot race the worker.

## Verified behaviour

| Check | Result |
|---|---|
| Network syscalls imported by either binary | none |
| DNS, TLS or HTTP libraries linked | none |
| Transcription inside an empty network namespace | completes normally |
| Audio retention on | three WAV files written |
| Audio retention off | none written, transcript still produced |
| Deletion | 61 files removed |
| Deletion with a symlink planted in `recordings/` | skipped, target outside the directory survived |
