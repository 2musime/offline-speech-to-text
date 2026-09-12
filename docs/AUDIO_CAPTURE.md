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
