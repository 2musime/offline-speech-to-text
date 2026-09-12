# Graphical interface

The Qt6 interface is built as `audio_to_text`. The recorder and Whisper pipeline
live in `audio_to_text_cli`; the GUI drives that worker with `QProcess` and
never loads Whisper itself, so the event loop stays free.

## Screens

Three, one job each:

| Screen | Holds | Reached by |
|---|---|---|
| **Home** | the application's name, what it does, and one button | where it opens; **← Home** from either other screen |
| **Recording** | settings, the record panel, the live transcript | **Start a Recording**, or `Ctrl+R` |
| **Saved transcripts** | the list and a reader | `Transcripts > Saved Transcripts`, or `Ctrl+H` |

Home is deliberately almost empty. It says what the application is and offers
the one thing someone opening it wants. **Recording does not happen here**: the
button moves to the recording screen, where the model, microphone and limit are
chosen first.

Leaving the recording screen is closed off while a recording or transcription is
running, for the same reason the library is: the controls for the thing still
running must not be hidden behind another screen.

## Menus

| Menu | Holds |
|---|---|
| **Home** | Quit, and nothing else |
| **Recording** | Start or stop, Cancel |
| **Transcripts** | Saved Transcripts, Delete All Recordings |
| **Help** | Privacy, About |

Saving and copying are not in a menu: they act on the transcript in front of
you, so they sit beside it, keeping their `Ctrl+S` and `Ctrl+Shift+C`
shortcuts. Deleting every recording is about stored data rather than the
application, so it sits with the transcripts.

## Layout

```text
File   Help                                          menu bar
Model [base.en v]  Microphone [default v]  Limit [15s v]  [x] Keep audio files
+-------------------------------------------------------------+
|  Start Recording   Stop Recording     00:23 / 01:00          |
|  Recording. Speak now.                                       |
|  ####################-------------------------              |
+-------------------------------------------------------------+
Transcript                                        [Save] [Copy]
+-------------------------------------------------------------+
|                                                             |
+-------------------------------------------------------------+
base (english, 141.1 MB) . Built-in Audio    Processed on this computer
```

Four decisions shape this:

**Secondary actions live in the menu bar.** Privacy and About sit under Help,
and deleting recordings under File. Deleting every recording is destructive and
belongs somewhere reached deliberately, not beside the microphone selector.

**Settings are sized to their contents.** A two-item model list stretched across
the window told nobody anything.

**Recording is one panel.** The button, the clock, the status line and the
progress bar describe a single thing, so they are grouped as one.

**Context lives in the status bar.** The model, input device and output path
were four stacked labels reading `Model: not loaded`, `Input: not selected`,
`Saving to: not known yet` before anything had happened, which looks like a list
of failures. They are now one line, showing only what is actually known.

The progress bar is hidden unless something is running: an empty bar on an idle
window suggests stalled work.

## Saved transcripts

`Transcripts > Saved Transcripts`, or `Ctrl+H`, replaces the window with the library: a
list of every transcript the application has written, newest first, each row
showing when it was recorded and the opening words. **Back to Recording** returns.

It is a separate screen rather than a panel beside the recorder. Nothing on it
can start a recording, because none of the recording controls are present: there
is no model selector, no limit, no record button and no progress bar. A control
that cannot be reached needs no rule about when it may be used.

Selecting a row reads that transcript into the pane on the right, headed with
when it was recorded, or `(audio deleted)` when the recording it came from is
gone. The reader has its own view, so nothing about browsing disturbs a live
transcription.

**The library cannot be opened while a recording or transcription is running.**
`Transcripts > Saved Transcripts` greys out until the worker finishes, and the recording
shortcuts are inert while the library is showing.

**Delete Transcript** sits in the bottom right corner, directly beneath the
transcript it would remove, so the target is never ambiguous. It is enabled only
while a transcript is on screen, and asks for confirmation naming the date
first.

Its colour is ember rather than plain red: destructive enough to give pause, but
not the colour the interface uses for something having gone wrong.

The recording the transcript came from is kept.
`File > Delete All Recordings` remains the only way to remove audio.

After a deletion the reader moves to a neighbouring transcript rather than
going blank: the row that takes the deleted one's place, or the row above when
the deleted one was last. Only when nothing is left does the reader empty and
the list show `No saved transcripts yet`.

Right-clicking a row additionally offers:

- **Copy** and **Save a Copy** of the transcript being read
- **Audio:** which recordings survive for that session, or `not kept` when
  retention was off or they have been deleted
- **Delete This Transcript** — the same action as the row control

The list is rebuilt when a new transcript is saved and after deleting
everything. With nothing stored it reads `No saved transcripts yet` rather than
showing an empty box.

Reading is confined the same way writing is: a path resolving outside the
application's own directory, or one that is a symbolic link, is refused rather
than followed. See [FILE_STORAGE.md](FILE_STORAGE.md).

## Keyboard

Every shortcut appears next to its menu entry, so it can be found rather than
memorised.

| Keys | Action |
|---|---|
| `Ctrl+R` | Start recording, or stop one in progress |
| `Esc` | Cancel whatever the worker is doing |
| `Ctrl+S` | Save the transcript to a file |
| `Ctrl+Shift+C` | Copy the whole transcript |
| `Ctrl+H` | Switch between recording and saved transcripts |
| `Up` / `Down` | Move through the saved transcripts |
| `Ctrl+Q` | Quit |

`Ctrl+R` is one action rather than two, so the same key both starts and stops.
Plain `Ctrl+C` is left to the transcript for copying a selection, which is why
copying the whole transcript takes the shifted form.

## State at a glance

A coloured dot beside the status line carries the current state: grey when idle,
red while recording, amber while working, green when finished, and red again on
a failure. Colour alone is never the only signal; the dot also carries a tooltip
and an accessible name naming the state, and the status line says the same thing
in words.

Disabled controls explain themselves. A greyed-out model selector says it cannot
be changed until the recording finishes; a greyed-out Save says it becomes
available once a transcription has finished. A control that is unavailable
without saying why is a dead end.

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
