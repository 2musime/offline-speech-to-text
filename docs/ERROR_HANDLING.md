# Error handling and resource ownership

Every operating-system resource in the CLI worker is owned by a scope. Failures
are reported under a category that the GUI turns into an actionable message.

## RAII ownership

| Type | Owns | Released by |
|---|---|---|
| `WhisperContext` | `whisper_context*` | `whisper_free` in the destructor |
| `CaptureDevice` | `ma_device` | `ma_device_stop` then `ma_device_uninit` |
| `ScopedThread` | streaming worker thread | `join` in the destructor |
| `std::ofstream` | output files | closed by the destructor |

`WhisperContext` is move-only. `CaptureDevice` is neither copyable nor movable
because `ma_device` stores back-pointers into itself.

The microphone is now released on every exit path, including early returns and
thrown exceptions. Previously a failure to open the capture device returned
without freeing the Whisper context.

`run()` holds the program logic and `main()` wraps it in a `try` block, so an
unexpected exception unwinds through the destructors and is reported as a
`WORKER` error rather than terminating the process.

## Error categories

Reports are written to stderr in a fixed, parseable shape:

```text
SEVERITY|CATEGORY|message
```

| Severity | Meaning | GUI behaviour |
|---|---|---|
| `ERROR` | The recording attempt failed | Show the cause, stop the worker, return to idle |
| `WARN` | Advisory; processing continues | Show in the status line |

Categories: `MICROPHONE`, `MODEL`, `RECORDING`, `TRANSCRIPTION`, `FILE_SAVING`,
`WORKER`.

Example:

```text
ERROR|MODEL|Could not load Whisper model: models/does-not-exist.bin
WARN|RECORDING|Recording reached the 15 second limit; extra audio was discarded.
```

This replaces the previous arrangement, where the GUI matched on prose such as
`"Could not open the microphone"`. Wording can now change without breaking the
interface between the two processes.

Command-line usage errors from `parse_options` are deliberately left uncategorised.
They are addressed to a person at a terminal, and the GUI supplies its own
arguments.

## Shutdown

`SIGINT` and `SIGTERM` set a `volatile sig_atomic_t` flag. The record loop checks
it, leaves the loop, and lets the destructors run.

The handler is installed with `sigaction` and **without** `SA_RESTART`, because a
blocking read must return when the signal lands. That alone is not sufficient:
libstdc++ retries `read` internally on `EINTR`, so `std::getline` can never be
interrupted by a signal. Commands are therefore read through `read_command`,
which calls `read` on the standard input descriptor and checks the shutdown flag
when it returns `EINTR`.

Verify with:

```bash
./build-release/audio_to_text_cli models/ggml-base.en.bin
# press Ctrl+C at the prompt
```

Expected output is `Shutting down.` and exit status `0`.

## Worker failures in the GUI

- A crashed worker (`QProcess::CrashExit`) reports that it stopped unexpectedly.
- A non-zero exit status reports the exit code.
- Closing the window during transcription asks the worker to quit, waits three
  seconds, then kills it.
- The reported cause stays on screen; it is not replaced by `Ready` when the
  worker exits.

## Checks

```bash
# Category reporting
./build-release/audio_to_text_cli models/does-not-exist.bin
# ERROR|MODEL|Could not load Whisper model: models/does-not-exist.bin

# Leaks and undefined behaviour
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DAUDIO_TO_TEXT_ENABLE_SANITIZERS=ON
cmake --build build-sanitize --parallel
printf 'q\n' | ASAN_OPTIONS=detect_leaks=1 \
  ./build-sanitize/audio_to_text_cli models/ggml-base.en.bin
```
