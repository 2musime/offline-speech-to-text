# Streaming transcription

Use `--stream` to transcribe audio while recording:

```bash
./build/audio_to_text models/ggml-small.en.bin --stream --threads 4
```

The streaming path:

1. Captures microphone samples into a preallocated single-producer ring buffer.
2. Keeps the audio callback free of locks and dynamic allocation.
3. Sends overlapping 5-second windows to a worker thread every 4 seconds.
4. Prints partial transcription and window latency while recording continues.
5. Drains the worker after recording stops.
6. Runs the existing final VAD, noise-reduction, and original-versus-cleaned transcription pass.

The overlap reduces word-boundary loss between windows. Partial text is informational and may repeat words at window boundaries; the
transcript file is written by the final pass.

The output also reports worker CPU time. If the ring buffer overflows, the application reports that audio was dropped; reduce the model size or thread count, or use normal mode for longer recordings.

Streaming is incompatible with `--compare` because model comparison is a batch benchmark:

```bash
./build/audio_to_text --compare \
  models/ggml-base.en.bin \
  models/ggml-small.en.bin \
  --threads 4
```
