# Graphical interface

The Qt6 interface is built as `audio_to_text`. The recorder and Whisper pipeline
live in `audio_to_text_cli`; the GUI drives that worker with `QProcess` and
never loads Whisper itself, so the event loop stays free.

## Build and run

```bash
sudo dnf install qt6-qtbase-devel
cmake -S . -B build-release
cmake --build build-release --parallel
./build-release/audio_to_text
```

## States

Every control's enabled state is a function of one enum and nothing else:

```text
Ready → LoadingModel → Recording → Stopping → Processing → Completed
                                                         ↘ Error
```

| State | Meaning | Start | Stop | Model, limit, mic | Save, copy |
|---|---|:--:|:--:|:--:|:--:|
| `Ready` | nothing running | yes | no | yes | no |
| `LoadingModel` | worker starting, model being validated | no | no | no | no |
| `Recording` | capturing, partials arriving | no | yes | no | no |
| `Stopping` | stop requested, capture winding down | no | no | no | no |
| `Processing` | VAD and final transcription running | no | no | no | no |
| `Completed` | transcript written | yes | no | yes | yes |
| `Error` | reported failure | yes | no | yes | if text arrived |

`apply_state` is the only function that calls `setEnabled`. Previously
enablement was set in four places that could disagree.

### Transitions are driven by the worker

The worker announces its progress, so the interface reflects what is actually
happening rather than what was requested:

| Worker line | Transition |
|---|---|
| `READY\|` | `LoadingModel` → `Recording`, and the clock starts |
| `PROCESSING\|` | `Stopping` → `Processing` |
| `SAVED\|TRANSCRIPT\|...` | → `Completed` |
| `ERROR\|...` | → `Error` |

`READY|` is emitted at every prompt, including after a completed transcription.
Only a transition out of `LoadingModel` starts a recording, so the later one
cannot begin a phantom second recording.

## Two recordings at once

`start_recording` returns immediately unless the state is `Ready`, `Completed`
or `Error`. The button is disabled in every other state as well, so a double
click, a stray shortcut, or a timer cannot launch a second worker.

## Model and device changes

The model, duration and microphone selectors are disabled from `LoadingModel`
until the worker finishes. Changing them mid-run would not affect the worker
already running, so accepting the change would misreport what produced the
transcript. The microphone selector additionally stays disabled until
enumeration finishes.

## Responsiveness

Two blocking calls were removed:

- device enumeration ran `waitForFinished(4000)` during construction, freezing
  the window for as long as the audio backend took to answer. It now runs
  asynchronously and fills the dropdown when it completes.
- starting the worker ran `waitForStarted(1000)`. The state machine waits for
  `READY|` instead.

Whisper has always run in the worker process, so inference never blocked the
event loop.

## Elapsed time and progress

Elapsed time uses `QElapsedTimer`, which is monotonic: unaffected by system
clock changes or by crossing midnight. The previous implementation subtracted
two `QTime::currentTime()` values and would have gone negative at midnight.

The clock starts when the worker reports `READY|`, not when the button is
pressed, so it does not count model loading as recorded audio.

The display shows elapsed against the limit, with a progress bar that fills
toward it. During `LoadingModel`, `Stopping` and `Processing` the bar is
indeterminate, because the worker reports no completion fraction for those
phases.

## Closing during a recording

Closing while `LoadingModel`, `Recording`, `Stopping` or `Processing` asks for
confirmation, because the transcript would be lost. On confirmation the worker
is asked to quit, given three seconds, then killed.

## Long recordings

Limits are 15, 45 and 60 seconds, and 5 or 10 minutes for dictation. `base.en`
is the default model because it transcribes roughly 3.5 times faster than
`small.en`, which is what decides whether a ten minute recording takes about a
minute to transcribe or several.

The final pass is chunked, and the worker reports each chunk as it finishes:

```text
PROGRESS|7|20
```

The interface turns that into a filling bar and a line naming what is left:

```text
Transcribing 7 of 20 (41s elapsed, about 76s left)
```

The estimate is derived from chunks already finished, so it appears only once
there is something to base it on.

**Cancelling.** The Stop button becomes **Cancel** while the worker is loading,
stopping or transcribing. No phase is a dead end: a ten minute recording can
take minutes to transcribe, and a wait with no way out is indistinguishable from
a hang.

## Live text

Partial text arrives as `PARTIAL|latency_ms|queue_seconds|text` and is merged by
word-level overlap; see [STREAMING_TRANSCRIPTION.md](STREAMING_TRANSCRIPTION.md).
The final pass replaces it with the saved transcript.
