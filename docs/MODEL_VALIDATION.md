# Model validation

Models are validated before the microphone is opened, so an unusable model fails
immediately instead of after a recording has been made.

## Checks

In order:

1. **Approved location.** The path is canonicalised and must resolve inside
   `./models`, `<data>/models`, or a `--model-dir` root. See
   [FILE_STORAGE.md](FILE_STORAGE.md).
2. **Regular file.** Directories, device nodes and sockets are refused.
3. **Plausible size.** Under 1 MiB the file is treated as truncated. The
   smallest published Whisper model is tens of megabytes, so this only catches
   partial downloads and stubs.
4. **Readable.** The file is opened for reading before anything else happens.
5. **ggml magic number.** The first four bytes must be `0x67676d6c` (`"ggml"`).
6. **Header sanity.** Vocabulary size must be plausible and the mel band count
   must be 80 or 128.
7. **Known geometry.** The audio layer count must map to a known model type.

## Stored metadata

Parsed from the header and kept for the run:

| Field | Source |
|---|---|
| Model type | audio layer count: 4 tiny, 6 base, 12 small, 24 medium, 32 large |
| Language coverage | multilingual when the vocabulary size is at least 51865 |
| Vocabulary size, mel bands, quantisation | header fields |
| File size and resolved path | filesystem |

Field order and the layer mapping were read from
`third_party/whisper.cpp/src/whisper.cpp`, not assumed.

The worker reports the result in parseable form, and the GUI displays it:

```text
MODEL|small|english|487614201|/home/you/project/models/ggml-small.en.bin
Model: small, English-only, 465.025 MB, 12 audio layers, 80 mel bands
```

## Warnings that do not stop the run

- the file name does not mention the model type its header reports
- the file name claims `.en.` but the header is multilingual, or the reverse
- the file is marked executable

The last is reported because it is unexpected, not because it is dangerous.
Model files are opened read-only and parsed as data. The application never
executes a model, passes one to a shell, or loads one as a library.

## Corrupted or incompatible models

| Condition | Message |
|---|---|
| Wrong magic number | `Not a ggml Whisper model...` |
| Under 1 MiB | `...probably truncated or incomplete` |
| Unknown layer count | `Unrecognised Whisper model geometry (N audio layers)...` |
| Implausible vocabulary or mel bands | `Model header is corrupted...` |
| Header valid but Whisper refuses it | `The model header is valid but Whisper could not load it...` |

The last case separates a damaged file from one built for a different
whisper.cpp version.

## Trusted sources

Models come from the whisper.cpp project's own distribution:

```text
https://huggingface.co/ggerganov/whisper.cpp
```

Download with the vendored script, which fetches from that repository:

```bash
bash third_party/whisper.cpp/models/download-ggml-model.sh small.en models
```

## Checksums

**Upstream publishes no checksum list.** Do not treat any hash in this document
as authoritative. The practical protection is to record the hash when a model is
first downloaded and confirm it does not change afterwards:

```bash
sha256sum models/*.bin > models/SHA256SUMS
sha256sum --check models/SHA256SUMS
```

For reference, the models on the machine where this was written hashed as:

```text
a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002  ggml-base.en.bin
c6138d6d58ecc8322097e0f987c32f1be8bb0a18532a3f88f734d1bbf9c41e5d  ggml-small.en.bin
```

These are an observation, not a published reference. Verify against a fresh
download over HTTPS from the source above if provenance matters to you.

## Checks

```bash
# Bad magic number
head -c 2000000 /dev/urandom > /tmp/ggml-garbage.bin
./build-release/audio_to_text_cli /tmp/ggml-garbage.bin --model-dir /tmp

# Truncated model
head -c 500 models/ggml-base.en.bin > /tmp/ggml-truncated.bin
./build-release/audio_to_text_cli /tmp/ggml-truncated.bin --model-dir /tmp
```
