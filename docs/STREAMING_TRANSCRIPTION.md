# Streaming transcription

Use `--stream` to transcribe while recording:

```bash
./build-release/audio_to_text_cli models/ggml-base.en.bin --stream
```

The streaming path:

1. Captures microphone samples into a bounded lock-free ring buffer. See
   [AUDIO_CAPTURE.md](AUDIO_CAPTURE.md).
2. Drains that buffer on a worker thread, never in the audio callback.
3. Transcribes overlapping 5-second windows, advancing 4 seconds at a time.
4. Emits a partial with its latency and how far behind live it is.
5. Drains the worker after recording stops.
6. Runs the final VAD and transcription pass over the whole recording.

Partial text is informational. The transcript file is written by the final pass,
which always sees every captured sample regardless of what the live path
skipped.

## Output

```text
PARTIAL|848|1.85|the quick brown fox jumps over
        ^    ^    ^
        |    |    text
        |    seconds of audio still waiting
        latency of this window, in milliseconds

FINAL|the quick brown fox jumps over the lazy dog
STREAMSTATS|emitted=5|rate_limited=0|dropped=0|worst_latency_ms=880|cpu_ms=33125
```

## Keeping up with slow transcription

Capture is never blocked by transcription: the audio callback only writes to the
ring buffer, and the worker drains on its own thread. When transcription cannot
keep pace, two mechanisms protect the live path, in order.

**Rate limiting.** No more than one partial per 750 ms. In steady state windows
arrive one per 4-second stride, far slower than this, so the limit never
applies. While catching up it does, and a limited window is skipped entirely
rather than merely silenced: the inference is not spent, and the audio is
already safe in the full-recording buffer.

The effect is that live text jumps forward to recent audio instead of lagging
further and further behind. Freshness is the right trade for text a person is
watching.

**Backlog cap.** Pending audio is held to one window plus three strides. Past
that, the oldest audio is discarded from the live path and counted as a dropped
window. This is a second line of defence; in practice rate limiting keeps the
backlog well under the cap.

Both counts are reported, and the GUI surfaces them:

```text
WARN|TRANSCRIPTION|2 streaming window(s) were discarded to keep up. Partial text
skipped ahead; the final transcription still covers the whole recording.
```

### Measured behaviour

25 to 30 seconds of audio on a 12-thread machine:

| Configuration | Emitted | Rate limited | Dropped | Worst latency | Peak queue |
|---|---:|---:|---:|---:|---:|
| `base.en`, 8 threads | 5 | 0 | 0 | 880 ms | 1.9 s |
| `small.en`, 1 thread | 3 | 4 | 0 | 10132 ms | 11.1 s |

The first row is normal operation: one partial per stride, under a second of
latency. The second is deliberately starved, and shows the live path shedding
more than half its windows to stay near live. Both produced a complete final
transcription.

## Overlap deduplication

Consecutive windows overlap by one second, so each partial repeats the tail of
the one before it. Merging is done on **whole words**, not characters: the
longest run where the tail of the existing text equals the head of the incoming
text is dropped, comparing letters and digits only so punctuation and
capitalisation differences between windows do not break the match.

The previous approach searched for a matching character suffix within the first
40 characters of the incoming text, which could resynchronise mid-word.

The merge logic lives in `partial_text.h`, separate from the widgets, and is
covered by cases including no overlap, full repetition, single-word overlap,
punctuation and case differences, and a repeated word that must not collapse
(`"very very good"` + `"good morning"` must not lose a word).

## Bounded growth

Live partial text is capped at 3000 words. Past that the front is trimmed and
the display is prefixed with `[earlier text trimmed]`. The recording buffer is
reserved to the duration limit up front and never grows beyond it, and the
worker's pending buffer is bounded by the backlog cap. Nothing in the streaming
path grows with recording length.

## Limits

Streaming cannot be combined with `--compare`, which is a batch benchmark.
