# Whisper model comparison

The application uses `base.en` as the default English model because it is a practical balance of speed, memory use, and accuracy for local CPU transcription.

## Models

| Model | Approximate file size | Expected speed | Expected accuracy |
|---|---:|---|---|
| `base.en` | 142 MB | Fast | Good |
| `small.en` | 466 MB | Slower | Better |

These are planning estimates. Use the benchmark output from this project for measurements on your Fedora machine.

## Download models

From the project root:

```bash
mkdir -p models
bash third_party/whisper.cpp/models/download-ggml-model.sh small.en models
```

The model files are intentionally ignored by Git.

## Compare models

Build and run the application in comparison mode:

```bash
cmake --build build -j2
./build/audio_to_text --compare \
  models/ggml-base.en.bin \
  models/ggml-small.en.bin \
  --threads 4
```

Record one utterance. The same raw recording and the same VAD speech segments are sent to every model. The output reports:

- model file size
- Whisper processing time
- peak resident memory of the process
- transcription text for manual accuracy comparison

Use the same fixed sentence and repeat the comparison for a fair result. Accuracy still requires a known reference transcript; compare each output against the words you intentionally spoke.

## Normal mode

With no model argument, the application uses `models/ggml-base.en.bin`:

```bash
./build/audio_to_text
```

A different model can be selected explicitly:

```bash
./build/audio_to_text models/ggml-small.en.bin --threads 4
```
