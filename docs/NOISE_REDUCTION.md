# Noise reduction

The recorder preserves the raw microphone input as `recording.wav` and writes a processed copy as `cleaned.wav`.

## Analysis

Before VAD and transcription, the application measures RMS energy in 20 ms frames. The noise floor is estimated from the quietest 20% of frames. It reports the RMS value, dBFS value, and attenuation threshold.

## Processing

Noise reduction is intentionally conservative. Samples below the guarded threshold are attenuated progressively instead of being hard-clipped. Speech timing and the full recording length are preserved, which keeps VAD segment positions valid and avoids aggressive speech distortion.

This is an amplitude gate, not a studio-quality spectral denoiser. It is appropriate as a first production-safe baseline. A later noise-reduction implementation should be evaluated against this baseline using the same recordings and reference transcripts.

## Transcription

In normal single-model mode, Whisper runs **once**, on the raw speech extracted
from `recording.wav`. The transcript and its processing time are printed, and
the transcript is saved to `transcription.txt`.

`cleaned.wav` is still produced as a diagnostic artifact so the noise floor and
attenuation can be inspected, but it is not transcribed in normal mode.

In `--compare` mode, each selected model still reports original and cleaned
transcripts with separate processing times, because that mode exists to measure
the difference.

Test with:

```bash
./build/audio_to_text models/ggml-small.en.bin --threads 4
```

Keep the original and cleaned audio, and compare both outputs against the words intentionally spoken. If cleaned output loses consonants or words, reduce the attenuation threshold or disable preprocessing for that recording.
