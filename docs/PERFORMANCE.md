# Transcription performance

## Supported hardware

| Requirement | Detail |
|---|---|
| Architecture | x86-64. Whisper and ggml build for other architectures, but nothing here has been tested on one |
| Operating system | Linux. Developed and tested on Fedora; the package targets Fedora and the build needs only GCC, CMake and Qt 6 |
| Processor | Any x86-64 CPU. Transcription is CPU only; no GPU backend is enabled in this build |
| Cores | Two are enough. Useful scaling stops around eight, see below |
| Memory | 1 GB free for `base.en`, 1.5 GB for `small.en`. Measured peaks are 260 MB and 670 MB; the rest is headroom |
| Disk | About 20 MB for the application, plus the model: 142 MB for `base.en` or 466 MB for `small.en` |
| Audio | Any capture device miniaudio can open through PulseAudio, PipeWire or ALSA |
| Network | None, ever, except when downloading a model |

The figures below come from one machine. Your own numbers are one command away,
and that is the honest way to know what to expect:

```bash
audio_to_text_cli --benchmark --input your-recording.wav models/ggml-base.en.bin
```

An RTF (real-time factor) below 1.0 means the machine transcribes faster than
the audio plays, which is what streaming needs. Every configuration measured
here is far below that, so slower hardware has considerable room before live
transcription stops keeping up.

## Reproducible benchmarking

`--benchmark` runs the production pipeline (VAD, then transcription) over a
fixed WAV file, so runs are comparable to each other:

```bash
./build-release/audio_to_text_cli --benchmark --input recording.wav \
  models/ggml-base.en.bin models/ggml-small.en.bin
```

The input must be mono 16-bit PCM at 16 kHz, the recorder's own format. Anything
else is refused with the format it actually found, rather than measured and
reported as if it were meaningful.

With no `--threads`, the sweep is 2, 4, 8 and 11 threads, skipping any count the
machine cannot run. With `--threads N`, only that count is measured.

Each row is also emitted in parseable form:

```text
PERF|model_load_ms=83|vad_us=368|transcription_ms=995|total_ms=1079|cpu_ms=7307|peak_memory_kb=268788|rtf=0.0818257
```

`rtf` is the real-time factor: transcription time divided by speech duration.
Below 1.0 the machine transcribes faster than the audio plays.

## Measurements

One run, 12.16 seconds of extracted speech, on a 12-thread x86-64 Linux machine.
Absolute numbers will differ on other hardware; the shape of the curve is the
part that transfers.

| Model | Threads | Load ms | Transcribe ms | Total ms | CPU ms | Peak KB | RTF |
|---|---:|---:|---:|---:|---:|---:|---:|
| base | 2 | 100 | 1585 | 1686 | 3181 | 250116 | 0.130 |
| base | 4 | 88 | 1332 | 1421 | 4908 | 256240 | 0.110 |
| base | 8 | 82 | 984 | 1067 | 7172 | 268588 | 0.081 |
| base | 11 | 81 | 920 | 1001 | 8928 | 268676 | 0.076 |
| small | 2 | 238 | 5523 | 5762 | 11134 | 665224 | 0.454 |
| small | 4 | 231 | 4676 | 4908 | 17905 | 672640 | 0.385 |
| small | 8 | 228 | 3490 | 3719 | 26721 | 672812 | 0.287 |
| small | 11 | 231 | 3158 | 3390 | 32426 | 672816 | 0.260 |

VAD takes 368 microseconds on this input: irrelevant next to inference.

Repeat runs vary by roughly 10% on an otherwise busy machine. Treat differences
below that as noise, and re-run before concluding anything from a small gap.

## What the numbers say

**Scaling stops being worth it after 8 threads.** For `base`, 4 to 8 threads cuts
transcription time by 26%. Eight to eleven cuts it by a further 7% while
spending 24% more CPU. `small` behaves the same way: 25% then 10%.

**Both models are comfortably real time.** Even `small` at 2 threads has an RTF
of 0.45, so a 5-second streaming window is transcribed in about 2.2 seconds.

**Memory depends on the model, not the thread count.** `base` sits near 260 MB
and `small` near 670 MB, varying by under 2% across the sweep.

**Model load is not worth optimising.** 80 ms for `base` and 230 ms for `small`,
paid once because the context is held open across recordings.

## Recommended defaults

| Setting | Default | Reason |
|---|---|---|
| Model | `base.en` | RTF 0.08 to 0.13, far below real time; `small.en` when accuracy matters more than latency |
| Threads | `min(8, cores - 1)` | The knee of the curve; more threads buy little and cost noticeably more CPU |

The previous default was `cores - 1`, which on this machine meant 11 threads:
the least efficient point measured. The GUI no longer passes `--threads` at all,
so the worker's default is the single source of truth.

Override when a particular run needs it:

```bash
./build-release/audio_to_text_cli models/ggml-small.en.bin --threads 11
```

## Allocation behaviour

Three allocation sites were removed from the transcription path:

- `run_transcription` copied each segment into an intermediate `int16` vector
  and then allocated a `float` vector for it. It now converts straight out of
  the source range into a `TranscriptionWorkspace` buffer that is reused across
  segments and across recordings.
- The streaming worker copied a full 5-second window out of its pending buffer
  on every stride. It now transcribes the window in place.
- The per-window `SpeechSegment` vector is hoisted out of the loop.

These matter most for the streaming path, which previously copied 160 KB every
four seconds, and for keeping the pipeline free of avoidable churn. They are not
a throughput win: at these sizes the time is dominated by inference, and the RTF
figures above are unchanged by them within run-to-run noise.

The Whisper context is created once and reused for every recording in a session,
so model load is paid once rather than per recording.
