# Audio capture reliability

## One bounded sink for every mode

Batch and streaming recording previously used different capture paths: batch
locked a mutex and appended to a `std::vector` inside the audio callback, while
streaming used a lock-free ring buffer. Both modes now share the ring buffer.

The audio callback does only this:

```cpp
const std::size_t written = capture->buffer.write(samples, frame_count);
capture->captured_frames.fetch_add(written, std::memory_order_relaxed);
if (written != frame_count) {
    capture->dropped_frames.fetch_add(frame_count - written, std::memory_order_relaxed);
}
```

No lock, no allocation, no system call, and no branch on recording mode. Moving
audio out of the buffer is `drain_capture`, which runs on the streaming worker
or on the main thread after the device stops, never in the callback. The
destination vector is reserved to the duration limit up front, so its `insert`
does not allocate either.

The ring buffer's capacity is the duration limit, so a full buffer and an
exceeded recording limit are the same condition, counted the same way.

## Dropped frames

`dropped_frames` counts frames the ring buffer could not accept. It is reported
in both frames and seconds:

```text
WARN|RECORDING|Reached the 15 second limit or could not keep up; 4800 frames (0.300000 s) were dropped.
```

A count above zero means either the recording hit its limit or the consumer
could not keep up with the device.

### Draining is unconditional

The drain always empties the ring buffer, including after the duration limit is
reached. Samples past the limit are consumed and discarded rather than left
behind.

This is not a detail. The streaming worker's loop runs while
`capture.buffer.available() > 0`. An earlier version stopped draining once the
kept samples reached the limit, so a recording that hit its limit left audio in
the buffer permanently: the worker spun forever, `join` never returned, the
final transcription never started, and the interface sat on a progress bar that
moved with nothing behind it. `tests/unit_tests.cpp` asserts the buffer is empty
after draining past the limit.

## Device disconnects and interruptions

The device is opened with a notification callback. A reroute, a system
interruption, or a stop that the application did not request all set
`device_lost`:

```cpp
case ma_device_notification_type_stopped:
    // miniaudio also raises this for our own ma_device_stop call.
    if (!capture->expected_stop.load(std::memory_order_acquire)) {
        capture->device_lost.store(true, std::memory_order_release);
    }
    break;
```

The `expected_stop` flag is what separates a deliberate stop from a disconnect;
without it every normal stop would look like a failure. A lost device is
reported as `ERROR|MICROPHONE|` and the recording is discarded rather than
transcribed, because its tail is not trustworthy.

## Silent and clipping input

A recording is checked before anything is transcribed.

**Every sample exactly zero** means no signal reached the application at all: a
working but quiet microphone still has a noise floor. This is almost always a
muted input, and it is reported as such rather than as "no speech detected",
which sends people looking for a better microphone when the one they have is
switched off:

```text
ERROR|MICROPHONE|No signal at all from "Built-in Audio Analog Stereo": every
sample was silence. The input is almost certainly muted. Unmute it in your
sound settings, or run: pactl set-source-mute @DEFAULT_SOURCE@ 0
```

**A peak below 32 of 32767** is reported as extremely quiet.

**More than 1% of samples at full scale** is reported as clipping. The loud
failure is as damaging to transcription as the quiet one and just as invisible,
since a clipped recording looks like a strong signal:

```text
WARN|MICROPHONE|Audio from "Built-in Audio Analog Stereo" is clipping: 65% of
samples are at full scale. Lower the input volume, or run:
pactl set-source-volume @DEFAULT_SOURCE@ 60%
```

## Permission and device failures

Every `ma_result` is translated before it reaches the user:

| Result | Message |
|---|---|
| `MA_ACCESS_DENIED` | permission was denied; grant microphone access |
| `MA_NO_DEVICE` | no capture device; connect a microphone |
| `MA_DEVICE_NOT_INITIALIZED` | device gone, likely disconnected |
| `MA_BUSY` | another application holds the microphone |
| anything else | the miniaudio description, verbatim |

## Input device selection

List the capture devices:

```bash
./build-release/audio_to_text_cli --list-devices
```

```text
DEVICE|0||Monitor of Built-in Audio Analog Stereo
DEVICE|1|default|Built-in Audio Analog Stereo
```

```text
DEVICE|0|default|Built-in Audio Analog Stereo|microphone
DEVICE|1||Monitor of Built-in Audio Analog Stereo|monitor
```

The last field separates a real input from a **monitor**, which records what is
played out of the speakers rather than what is said. Selecting one produces a
recording of silence on a quiet machine, which is indistinguishable from a
broken microphone. Real inputs are listed first, the system default first of
all, and monitors last; choosing one is reported as a warning. Detection is by
name, because miniaudio does not distinguish them.

An index that no longer exists no longer refuses to record. Devices are plugged
in and unplugged while the application runs, so a stale index falls back to the
system default and says so.

Select one by index, or omit `--device` for the system default:

```bash
./build-release/audio_to_text_cli models/ggml-base.en.bin --device 1
```

Devices are enumerated and opened from the same `ma_context`, so the identifier
stays valid between listing and opening. An out-of-range index is refused before
the model is loaded.

The active device is announced once the device opens:

```text
INPUT|Built-in Audio Analog Stereo
```

The GUI populates its microphone dropdown by running `--list-devices` at
startup, passes the chosen index as `--device`, and shows the device the worker
actually opened in its `Input:` label. That matters because the name the backend
reports can differ from the one that was requested.

## Checks

```bash
# Enumeration
./build-release/audio_to_text_cli --list-devices

# Out-of-range index
./build-release/audio_to_text_cli models/ggml-base.en.bin --device 99
# ERROR|MICROPHONE|Capture device 99 does not exist; 2 device(s) are available.
```

The ring buffer was additionally exercised with a standalone producer/consumer
harness under AddressSanitizer and UndefinedBehaviorSanitizer, moving 30 seconds
of audio per run and verifying every sample arrived in order. ThreadSanitizer
would be the stronger check; its runtime was not installed on the machine where
this was written.
