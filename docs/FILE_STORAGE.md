# File storage

The application no longer writes to fixed filenames in the working directory.
Every artefact goes to a directory the application owns, under a unique name,
through an atomic write.

## Data directory

On Linux, resolved in this order:

1. `$XDG_DATA_HOME/audio-to-text`
2. `$HOME/.local/share/audio-to-text`
3. `./.audio-to-text` if neither variable is set

On Windows:

1. `%LOCALAPPDATA%\audio-to-text`
2. `.\.audio-to-text` if the variable is not set

`LOCALAPPDATA` rather than `APPDATA`, because recordings are large and
machine-local and should not follow the user onto another machine.

Layout:

```text
<data>/recordings/     WAV artefacts
<data>/transcripts/    transcription text
<data>/models/         optional approved model location
```

On Linux, directories are created with mode `0700` and files with mode `0600`,
so output is readable only by the user who produced it. Windows has no
equivalent single call; `%LOCALAPPDATA%` already carries a per-user ACL that a
directory created beneath it inherits, so the result is the same without an
explicit step. The worker prints the resolved data directory at startup.

## Unique filenames

Each recording gets a stamp of the form `YYYYMMDD-HHMMSS-XXXX`, where `XXXX` is
random. All artefacts of one recording share it:

```text
20260912-161107-6ac4-recording.wav
20260912-161107-6ac4-cleaned.wav
20260912-161107-6ac4-speech.wav
20260912-161107-6ac4-transcription.txt
```

Recordings are no longer overwritten by the next one.

## Atomic writes

`AtomicFile` writes to `<target>.tmp-<pid>`, flushes it, then renames it into
place. A reader therefore sees either no file or a complete one, never a
half-written one. If the write fails or the process exits early, the destructor
removes the temporary file.

The temporary is always a sibling of its target, because the rename is only
atomic within one volume.

| Step | Linux | Windows |
| --- | --- | --- |
| Create | `open(O_CREAT\|O_EXCL\|O_NOFOLLOW)` | `_wopen(_O_CREAT\|_O_EXCL\|_O_BINARY)` |
| Flush | `fsync` | `_commit` |
| Rename | `rename` | `std::filesystem::rename` |
| Durable rename | `fsync` on the parent directory | no equivalent |

Two differences are worth naming:

- POSIX `rename` replaces an existing target; the Windows call it maps to fails
  unless replacement is requested, so the replace is explicit there. A unit
  test commits twice over the same path to hold this in place.
- Windows offers no way to flush a directory, so only the file contents are
  made durable, not the rename itself.

`O_EXCL` with `O_CREAT` refuses any existing name — a symbolic link on Linux, a
reparse point on Windows — so an attacker who plants a link at the temporary's
path cannot redirect the write.

## Path validation

Before any write, the destination must satisfy both:

- it resolves inside the data directory, checked with `weakly_canonical`, so a
  symlinked subdirectory pointing elsewhere is refused
- it is not itself a symbolic link

Components are compared literally on Linux and case-insensitively on Windows,
matching what each filesystem itself considers the same name. Folding on
Windows cannot admit a path that is genuinely outside the root, because Windows
does not allow two directories whose names differ only in case.

Failures are reported as `ERROR|FILE_SAVING|...` and the recording stops.

## Approved model directories

Model files must resolve inside one of:

- `./models` relative to the working directory
- `<data>/models`
- any directory passed with `--model-dir`

The path is canonicalised first, so `../../etc/passwd` and a model reached
through a symbolic link both land outside the approved roots and are refused:

```text
$ ./build-release/audio_to_text_cli /tmp/model.bin
ERROR|MODEL|Model is outside the approved directories (...): /tmp/model.bin.
Pass --model-dir to approve another directory.

$ ./build-release/audio_to_text_cli /tmp/model.bin --model-dir /tmp
```

The model is also required to be an existing regular file. Model files are read
as data and are never executed.

## Reporting saved files

Because names are now generated, the worker announces each artefact in a
parseable form on stdout:

```text
SAVED|RECORDING|/home/you/.local/share/audio-to-text/recordings/...-recording.wav
SAVED|CLEANED|...
SAVED|SPEECH|...
SAVED|TRANSCRIPT|...
```

The GUI keys completion off `SAVED|TRANSCRIPT|` and offers that path as the
default when saving a copy. This replaces matching on the old fixed string
`Saved transcription.txt`.

## Checks

```bash
# Refuses a model outside the approved roots
./build-release/audio_to_text_cli /tmp/model.bin

# Permissions on produced files
stat -c '%a %n' ~/.local/share/audio-to-text/recordings/*
```
