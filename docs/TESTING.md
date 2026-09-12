# Testing

```bash
cmake -S . -B build-release
cmake --build build-release --parallel
cd build-release && ctest --output-on-failure
```

Three suites, registered with CTest:

| Suite | Binary | Covers |
|---|---|---|
| `unit` | `audio_to_text_unit_tests` | audio, WAV, storage and model logic |
| `gui` | `audio_to_text_gui_tests` | state transitions and partial-text merging |
| `cli` | `tests/cli_tests.sh` | argument handling, shutdown, reporting protocol |

The `cli` suite needs `models/ggml-base.en.bin`. It skips with a message rather
than failing when the model is absent, so a fresh checkout does not report a
false failure.

## Structure this required

`main.cpp` was 2139 lines containing `main` and the miniaudio implementation, so
none of its logic could be reached from a test binary. The pure logic now lives
in headers that both the application and the tests include:

| Header | Contents |
|---|---|
| `diagnostics.h` | error categories, the `SEVERITY\|CATEGORY\|message` protocol |
| `audio_ring_buffer.h` | the lock-free capture buffer |
| `file_storage.h` | data directory, path validation, `AtomicFile` |
| `model_info.h` | model resolution and ggml header inspection |
| `wav_io.h` | WAV reading and writing |
| `speech_detection.h` | noise floor, attenuation, voice activity detection |
| `ui_state.h` | the interface state machine and its control table |
| `partial_text.h` | overlap merging for streaming partials |

`main.cpp` is now 1421 lines and holds the device, worker and command loop.
The move was verbatim; no behaviour changed.

The test harness is `tests/test_harness.h`, about sixty lines. The project has
no external dependencies and a test framework would have been its first.

## Coverage against the plan

| Required | Where |
|---|---|
| WAV header generation | `unit` — every header field, plus a read-back round trip |
| WAV size limits | `unit` — zero sample rate, overflow boundary, empty payload |
| Empty audio | `unit` — noise floor, detection, extraction, attenuation |
| Silent audio | `unit` — zero floor, no segments, length preserved |
| Noise-floor calculation | `unit` — tracks amplitude, guarded minimum, loud peaks survive |
| VAD boundaries | `unit` — padding, ordering, in-range, no-contrast case |
| Overlapping segment handling | `unit` — merging, no overlap, 30 s chunking |
| Invalid model paths | `unit` + `cli` — missing, directory, traversal, outside roots |
| Invalid thread counts | `cli` — zero, negative, non-numeric, overflowing, absent |
| Ring-buffer overflow | `unit` — capacity, wrap, reuse, and a threaded ordering check |
| Worker shutdown | `cli` — SIGINT and the quit command, exit status and message |
| File-write failures | `unit` — missing directory, abandoned write, confinement, symlinks |
| Model selection | `unit` + `cli` — layer counts, vocabulary, corrupt headers |
| GUI state transitions | `gui` — the full control table for all seven states |

## Notes on two tests

**Voice detection needs contrast.** The noise floor is the 20th-percentile frame
energy, so an input that is loud throughout has a floor equal to its own level
and a threshold above it. A continuous tone with no silence is correctly
detected as no speech. A first version of the chunking test used exactly that
input and failed; the test was wrong, not the detector. The behaviour is now
asserted directly so the property is documented rather than rediscovered.

**The CLI suite resolves paths from its own location.** It passed standalone and
failed under CTest, which runs it from the build tree. It now derives the
project root from `$0` and works from any working directory.

## Sanitizers

```bash
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DAUDIO_TO_TEXT_ENABLE_SANITIZERS=ON
cmake --build build-sanitize --parallel
./build-sanitize/audio_to_text_unit_tests
QT_QPA_PLATFORM=offscreen ./build-sanitize/audio_to_text_gui_tests
```

Both suites pass clean under AddressSanitizer and UndefinedBehaviorSanitizer.

ThreadSanitizer would be the right tool for the ring buffer's concurrency check
and is not used here: `libtsan` was not installed on the machine where this was
written. The threaded test verifies that every sample crosses the buffer in
order, which catches loss and corruption but is weaker than a race detector.

## Not covered

- No test drives real widgets. The state table and text merging are tested as
  pure functions; clicking Start and observing the window is not automated.
- Nothing exercises a real microphone, a device disconnect, or a permission
  refusal. Those need hardware the test cannot control.
- Transcription accuracy is not asserted. `--benchmark` measures speed against
  fixed audio; see [PERFORMANCE.md](PERFORMANCE.md).
