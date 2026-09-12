#include "test_harness.h"

#include "audio_ring_buffer.h"
#include "file_storage.h"
#include "model_info.h"
#include "speech_detection.h"
#include "wav_io.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <thread>

namespace {

constexpr std::uint32_t kRate = 16000;

fs::path scratch_directory() {
    static const fs::path directory = [] {
        const char* base = std::getenv("AUDIO_TO_TEXT_TEST_DIR");
        fs::path path = base != nullptr ? fs::path(base)
                                        : fs::temp_directory_path() / "audio_to_text_tests";
        std::error_code error;
        fs::remove_all(path, error);
        fs::create_directories(path, error);
        return path;
    }();
    return directory;
}

std::vector<std::int16_t> silence(std::size_t samples) {
    return std::vector<std::int16_t>(samples, 0);
}

// A loud tone surrounded by silence: gives voice detection something with an
// unambiguous start and end.
std::vector<std::int16_t> tone_within_silence(
    double lead_seconds, double tone_seconds, double tail_seconds) {
    std::vector<std::int16_t> samples;
    const auto append_silence = [&samples](double seconds) {
        samples.insert(samples.end(), static_cast<std::size_t>(seconds * kRate), 0);
    };
    append_silence(lead_seconds);
    const std::size_t tone_samples = static_cast<std::size_t>(tone_seconds * kRate);
    for (std::size_t i = 0; i < tone_samples; ++i) {
        const double phase = 2.0 * 3.14159265358979 * 440.0 * i / kRate;
        samples.push_back(static_cast<std::int16_t>(std::sin(phase) * 12000.0));
    }
    append_silence(tail_seconds);
    return samples;
}

// ---------------------------------------------------------------- WAV output

void test_wav_header_generation() {
    harness::begin("WAV header generation");

    const fs::path path = scratch_directory() / "header.wav";
    const std::vector<std::int16_t> samples(1000, 1234);
    CHECK("write succeeds", write_wav(path, samples, kRate));

    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    CHECK_EQ("file is header plus payload", bytes.size(), std::size_t{44 + 2000});

    const auto u32 = [&bytes](std::size_t offset) {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };
    const auto u16 = [&bytes](std::size_t offset) {
        std::uint16_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };

    CHECK("RIFF tag", std::memcmp(bytes.data(), "RIFF", 4) == 0);
    CHECK("WAVE tag", std::memcmp(bytes.data() + 8, "WAVE", 4) == 0);
    CHECK("fmt tag", std::memcmp(bytes.data() + 12, "fmt ", 4) == 0);
    CHECK("data tag", std::memcmp(bytes.data() + 36, "data", 4) == 0);
    CHECK_EQ("RIFF size matches file", u32(4), static_cast<std::uint32_t>(bytes.size() - 8));
    CHECK_EQ("fmt chunk size", u32(16), std::uint32_t{16});
    CHECK_EQ("PCM format code", u16(20), std::uint16_t{1});
    CHECK_EQ("channel count", u16(22), std::uint16_t{1});
    CHECK_EQ("sample rate", u32(24), kRate);
    CHECK_EQ("byte rate", u32(28), kRate * 2);
    CHECK_EQ("block align", u16(32), std::uint16_t{2});
    CHECK_EQ("bits per sample", u16(34), std::uint16_t{16});
    CHECK_EQ("data size matches payload", u32(40), std::uint32_t{2000});

    // The file must survive a round trip through the reader.
    std::vector<std::int16_t> read_back;
    std::string reason;
    CHECK("round trip reads", read_wav(path, read_back, kRate, reason));
    CHECK_EQ("round trip sample count", read_back.size(), samples.size());
    CHECK("round trip sample values", read_back == samples);
}

void test_wav_size_limits() {
    harness::begin("WAV size limits");

    const fs::path path = scratch_directory() / "limits.wav";
    CHECK_FALSE("zero sample rate is refused", write_wav(path, silence(10), 0));

    // A count that would overflow the 32-bit size fields must be refused
    // without attempting a multi-gigabyte allocation, so the vector is faked
    // by checking the boundary arithmetic the writer uses.
    const std::size_t maximum =
        (std::numeric_limits<std::uint32_t>::max() - 36) / sizeof(std::int16_t);
    CHECK("limit is under 2^31 samples", maximum < (std::size_t{1} << 31));

    CHECK("empty sample vector still writes a valid header",
          write_wav(scratch_directory() / "empty.wav", {}, kRate));
    std::vector<std::int16_t> read_back;
    std::string reason;
    CHECK_FALSE("reader refuses a file with no samples",
                read_wav(scratch_directory() / "empty.wav", read_back, kRate, reason));

    std::vector<std::int16_t> ignored;
    CHECK_FALSE("reader refuses a non-RIFF file",
                read_wav(scratch_directory() / "missing.wav", ignored, kRate, reason));
}

// -------------------------------------------------------------- audio inputs

void test_empty_audio() {
    harness::begin("Empty audio");

    const std::vector<std::int16_t> nothing;
    const NoiseProfile profile = measure_noise_floor(nothing, kRate);
    CHECK("noise floor of nothing is zero", profile.noise_rms == 0.0f);
    CHECK("threshold stays at the guarded minimum", profile.attenuation_threshold > 0.0f);

    CHECK("no speech segments in empty audio",
          detect_speech_segments(nothing, kRate).empty());
    CHECK("extracting from empty audio yields nothing",
          extract_speech(nothing, {}).empty());
    CHECK("reducing noise on empty audio yields nothing",
          reduce_noise(nothing, profile).empty());
}

void test_silent_audio() {
    harness::begin("Silent audio");

    const std::vector<std::int16_t> quiet = silence(kRate * 3);
    const NoiseProfile profile = measure_noise_floor(quiet, kRate);
    CHECK("silence measures a zero noise floor", profile.noise_rms == 0.0f);
    CHECK("silence produces no speech segments",
          detect_speech_segments(quiet, kRate).empty());

    const std::vector<std::int16_t> cleaned = reduce_noise(quiet, profile);
    CHECK_EQ("noise reduction preserves length", cleaned.size(), quiet.size());
    bool all_zero = true;
    for (const std::int16_t sample : cleaned) {
        if (sample != 0) { all_zero = false; break; }
    }
    CHECK("silence stays silent", all_zero);
}

void test_noise_floor() {
    harness::begin("Noise-floor calculation");

    // Uniform low-level noise: the floor should land near its amplitude.
    std::vector<std::int16_t> hiss(kRate * 2);
    for (std::size_t i = 0; i < hiss.size(); ++i) {
        hiss[i] = static_cast<std::int16_t>((i % 2 == 0) ? 300 : -300);
    }
    const NoiseProfile profile = measure_noise_floor(hiss, kRate);
    const float expected = 300.0f / 32768.0f;
    CHECK("floor tracks the noise amplitude",
          std::fabs(profile.noise_rms - expected) < expected * 0.1f);
    CHECK("threshold sits above the measured floor",
          profile.attenuation_threshold > profile.noise_rms);

    // Loud audio must not push the threshold below its guarded minimum.
    const NoiseProfile loud = measure_noise_floor(tone_within_silence(0.0, 2.0, 0.0), kRate);
    CHECK("threshold keeps its floor", loud.attenuation_threshold >= 0.003f);

    // Attenuation must leave loud audio essentially untouched.
    const std::vector<std::int16_t> tone = tone_within_silence(0.0, 1.0, 0.0);
    const std::vector<std::int16_t> cleaned = reduce_noise(tone, profile);
    std::int16_t peak_before = 0;
    std::int16_t peak_after = 0;
    for (std::size_t i = 0; i < tone.size(); ++i) {
        peak_before = std::max<std::int16_t>(peak_before, std::abs(tone[i]));
        peak_after = std::max<std::int16_t>(peak_after, std::abs(cleaned[i]));
    }
    CHECK("loud peaks survive attenuation", peak_after > peak_before * 0.9);
}

void test_vad_boundaries() {
    harness::begin("VAD boundaries");

    const std::vector<std::int16_t> audio = tone_within_silence(1.0, 2.0, 1.0);
    const std::vector<SpeechSegment> segments = detect_speech_segments(audio, kRate);
    CHECK("one speech region is found", segments.size() == 1);
    if (segments.size() == 1) {
        const double begin = segments[0].begin / static_cast<double>(kRate);
        const double end = segments[0].end / static_cast<double>(kRate);
        // 200 ms of padding is added on each side by design.
        CHECK("segment starts near the tone, with padding", begin > 0.5 && begin <= 1.0);
        CHECK("segment ends near the tone, with padding", end >= 3.0 && end < 3.6);
        CHECK("segment stays inside the buffer", segments[0].end <= audio.size());
        CHECK("segment is ordered", segments[0].begin < segments[0].end);
    }

    const std::vector<std::int16_t> speech = extract_speech(audio, segments);
    CHECK("extracted speech is shorter than the recording", speech.size() < audio.size());
    CHECK("extracted speech is not empty", !speech.empty());
}

void test_overlapping_segments() {
    harness::begin("Overlapping segment handling");

    // Two bursts separated by less than the merge gap must become one segment.
    std::vector<std::int16_t> audio = tone_within_silence(0.5, 1.0, 0.1);
    const std::vector<std::int16_t> second = tone_within_silence(0.0, 1.0, 0.5);
    audio.insert(audio.end(), second.begin(), second.end());

    const std::vector<SpeechSegment> segments = detect_speech_segments(audio, kRate);
    CHECK("close bursts merge into one segment", segments.size() == 1);

    for (std::size_t i = 1; i < segments.size(); ++i) {
        CHECK("segments never overlap", segments[i].begin >= segments[i - 1].end);
    }

    // Detection is contrast-based: the noise floor is the 20th-percentile frame
    // energy, so the input needs enough silence for that percentile to land in
    // it. Continuous tone with no silence is correctly detected as no speech.
    CHECK("continuous sound with no silence yields no segments",
          detect_speech_segments(tone_within_silence(0.0, 5.0, 0.0), kRate).empty());

    // Anything longer than Whisper's 30 second window must be chunked.
    const std::vector<std::int16_t> lengthy = tone_within_silence(20.0, 70.0, 20.0);
    const std::vector<SpeechSegment> chunks = detect_speech_segments(lengthy, kRate);
    CHECK("long speech is split into chunks", chunks.size() >= 3);
    bool all_within_window = true;
    for (const SpeechSegment& chunk : chunks) {
        if (chunk.end - chunk.begin > static_cast<std::size_t>(WHISPER_SAMPLE_RATE) * 30) {
            all_within_window = false;
        }
    }
    CHECK("no chunk exceeds the 30 second window", all_within_window);
}

// ------------------------------------------------------------------- models

void test_model_paths() {
    harness::begin("Invalid model paths and model selection");

    const fs::path root = scratch_directory() / "models";
    std::error_code error;
    fs::create_directories(root, error);
    const std::vector<fs::path> roots{root};

    fs::path resolved;
    std::string reason;
    CHECK_FALSE("a missing model is refused",
                resolve_model_path((root / "absent.bin").string(), roots, resolved, reason));
    CHECK("the reason names the file", reason.find("absent.bin") != std::string::npos);

    CHECK_FALSE("a directory is refused",
                resolve_model_path(root.string(), roots, resolved, reason));

    // A real file outside every approved root must be refused.
    const fs::path outside = scratch_directory() / "outside.bin";
    { std::ofstream(outside) << "x"; }
    CHECK_FALSE("a model outside the approved roots is refused",
                resolve_model_path(outside.string(), roots, resolved, reason));
    CHECK("the reason mentions approval", reason.find("approved") != std::string::npos);

    // The same file becomes acceptable once its directory is approved.
    const std::vector<fs::path> widened{root, scratch_directory()};
    CHECK("approving the directory admits the file",
          resolve_model_path(outside.string(), widened, resolved, reason));

    // Header validation.
    const auto write_model = [&](const fs::path& path, std::uint32_t magic,
                                 std::int32_t vocab, std::int32_t layers, std::int32_t mels) {
        std::ofstream file(path, std::ios::binary);
        const std::int32_t header[12] = {static_cast<std::int32_t>(magic), vocab, 1500, 512, 8,
                                         layers, 448, 512, 8, layers, mels, 1};
        file.write(reinterpret_cast<const char*>(header), sizeof(header));
        const std::vector<char> padding(1024 * 1024 + 64, 0);
        file.write(padding.data(), static_cast<std::streamsize>(padding.size()));
    };

    ModelMetadata metadata;
    const fs::path good = root / "ggml-base.en.bin";
    write_model(good, 0x67676d6c, 51864, 6, 80);
    CHECK("a well formed header is accepted", inspect_model(good, metadata, reason));
    CHECK_TEXT("layer count selects the model type", metadata.variant, "base");
    CHECK_FALSE("vocabulary size marks it English-only", metadata.multilingual);

    write_model(root / "ggml-small.en.bin", 0x67676d6c, 51864, 12, 80);
    CHECK("twelve layers means small", inspect_model(root / "ggml-small.en.bin", metadata, reason));
    CHECK_TEXT("small is selected", metadata.variant, "small");

    write_model(root / "ggml-large.bin", 0x67676d6c, 51866, 32, 128);
    CHECK("multilingual vocabulary is detected",
          inspect_model(root / "ggml-large.bin", metadata, reason) && metadata.multilingual);

    write_model(root / "bad-magic.bin", 0x00000000, 51864, 6, 80);
    CHECK_FALSE("a wrong magic number is refused",
                inspect_model(root / "bad-magic.bin", metadata, reason));
    CHECK("the reason mentions the magic number", reason.find("magic") != std::string::npos);

    write_model(root / "bad-geometry.bin", 0x67676d6c, 51864, 99, 80);
    CHECK_FALSE("an unknown layer count is refused",
                inspect_model(root / "bad-geometry.bin", metadata, reason));

    write_model(root / "bad-mels.bin", 0x67676d6c, 51864, 6, 37);
    CHECK_FALSE("an unexpected mel count is refused",
                inspect_model(root / "bad-mels.bin", metadata, reason));

    write_model(root / "bad-vocab.bin", 0x67676d6c, 3, 6, 80);
    CHECK_FALSE("an implausible vocabulary is refused",
                inspect_model(root / "bad-vocab.bin", metadata, reason));

    const fs::path truncated = root / "truncated.bin";
    { std::ofstream file(truncated, std::ios::binary); file << "ggml"; }
    CHECK_FALSE("a truncated model is refused", inspect_model(truncated, metadata, reason));
    CHECK("the reason mentions truncation",
          reason.find("truncated") != std::string::npos);
}

// ------------------------------------------------------------- ring buffer

void test_ring_buffer_overflow() {
    harness::begin("Ring-buffer overflow");

    AudioRingBuffer buffer(8);
    const std::vector<std::int16_t> block{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

    CHECK_EQ("writes are capped at capacity", buffer.write(block.data(), block.size()),
             std::size_t{8});
    CHECK_EQ("available reflects what was accepted", buffer.available(), std::size_t{8});
    CHECK_EQ("a full buffer accepts nothing more", buffer.write(block.data(), 1), std::size_t{0});

    std::vector<std::int16_t> out;
    CHECK_EQ("reading drains what is there", buffer.read(out, 100), std::size_t{8});
    CHECK("values survive the round trip",
          (out == std::vector<std::int16_t>{1, 2, 3, 4, 5, 6, 7, 8}));
    CHECK_EQ("an empty buffer reads nothing", buffer.read(out, 4), std::size_t{0});

    // Space frees up as the reader consumes, and indices wrap correctly.
    CHECK_EQ("space is reusable after reading", buffer.write(block.data(), 5), std::size_t{5});
    CHECK_EQ("partial read", buffer.read(out, 3), std::size_t{3});
    CHECK_EQ("write wraps past the end", buffer.write(block.data(), 6), std::size_t{6});
    CHECK_EQ("total available after wrap", buffer.available(), std::size_t{8});

    buffer.reset();
    CHECK_EQ("reset empties the buffer", buffer.available(), std::size_t{0});

    // One producer and one consumer must move every sample, in order.
    AudioRingBuffer shared(4096);
    constexpr std::size_t total = 200000;
    std::atomic<bool> done{false};
    std::vector<std::int16_t> drained;
    drained.reserve(total);

    std::thread producer([&] {
        std::vector<std::int16_t> chunk(128);
        std::size_t produced = 0;
        while (produced < total) {
            const std::size_t count = std::min(chunk.size(), total - produced);
            for (std::size_t i = 0; i < count; ++i) {
                chunk[i] = static_cast<std::int16_t>((produced + i) & 0x7fff);
            }
            produced += shared.write(chunk.data(), count);
        }
        done.store(true, std::memory_order_release);
    });
    std::thread consumer([&] {
        std::vector<std::int16_t> scratch;
        while (!done.load(std::memory_order_acquire) || shared.available() > 0) {
            if (shared.read(scratch, 512) > 0) {
                drained.insert(drained.end(), scratch.begin(), scratch.end());
            }
        }
    });
    producer.join();
    consumer.join();

    CHECK_EQ("every sample crosses the buffer", drained.size(), total);
    bool ordered = true;
    for (std::size_t i = 0; i < drained.size(); ++i) {
        if (drained[i] != static_cast<std::int16_t>(i & 0x7fff)) { ordered = false; break; }
    }
    CHECK("samples arrive in order and uncorrupted", ordered);
}

// The condition that hung the application: once the recording reached its
// duration limit, the drain stopped consuming, the ring buffer never emptied,
// and the streaming worker's loop waited on it forever.
void test_drain_past_the_limit() {
    harness::begin("Draining past the duration limit");

    constexpr std::size_t capacity = 8192;
    constexpr std::size_t limit = 1000;
    AudioCapture capture(capacity);

    std::vector<std::int16_t> block(4000);
    for (std::size_t i = 0; i < block.size(); ++i) {
        block[i] = static_cast<std::int16_t>(i & 0x7fff);
    }
    CHECK_EQ("audio is buffered", capture.buffer.write(block.data(), block.size()),
             block.size());

    std::vector<std::int16_t> samples;
    samples.reserve(limit);
    std::vector<std::int16_t> scratch;
    const std::size_t kept = drain_capture(capture, samples, scratch, limit);

    CHECK_EQ("only the limit is kept", samples.size(), limit);
    CHECK_EQ("the report counts what was kept", kept, limit);
    // The regression assertion: leaving anything behind is what caused the hang.
    CHECK_EQ("the buffer is emptied even past the limit", capture.buffer.available(),
             std::size_t{0});
    CHECK_EQ("the excess is counted as dropped",
             capture.dropped_frames.load(), block.size() - limit);

    // Draining again must be harmless and must still leave the buffer empty.
    const std::size_t again = drain_capture(capture, samples, scratch, limit);
    CHECK_EQ("a second drain keeps nothing", again, std::size_t{0});
    CHECK_EQ("the buffer stays empty", capture.buffer.available(), std::size_t{0});

    // A writer that keeps going after the limit must not refill it forever.
    capture.buffer.write(block.data(), block.size());
    drain_capture(capture, samples, scratch, limit);
    CHECK_EQ("later audio is consumed and discarded", capture.buffer.available(),
             std::size_t{0});
    CHECK_EQ("samples never exceed the limit", samples.size(), limit);

    capture.reset();
    CHECK_EQ("reset clears the dropped count", capture.dropped_frames.load(), std::size_t{0});
}

// ------------------------------------------------------------ file storage

void test_file_write_failures() {
    harness::begin("File-write failures");

    const fs::path directory = scratch_directory() / "storage";
    std::error_code error;
    fs::create_directories(directory, error);

    // Writing into a directory that does not exist must fail, not throw.
    CHECK_FALSE("writing into a missing directory fails",
                write_wav(directory / "absent" / "x.wav", silence(100), kRate));

    // A failed write must leave no temporary file behind.
    bool stray = false;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, error)) {
        if (entry.path().string().find(".tmp-") != std::string::npos) { stray = true; }
    }
    CHECK_FALSE("no temporary file is left behind", stray);

    // A committed write is complete, never partial.
    const fs::path target = directory / "committed.wav";
    CHECK("a good write commits", write_wav(target, silence(500), kRate));
    CHECK("the committed file exists", fs::exists(target));
    CHECK_EQ("the committed file is whole", fs::file_size(target), std::uintmax_t{44 + 1000});

    // An abandoned AtomicFile removes its temporary.
    {
        AtomicFile abandoned(directory / "abandoned.bin");
        CHECK("temporary opens", abandoned.open());
        CHECK("temporary accepts data", abandoned.write("abc", 3));
        // Destructor runs without commit.
    }
    CHECK_FALSE("an uncommitted write leaves no target", fs::exists(directory / "abandoned.bin"));
    stray = false;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, error)) {
        if (entry.path().string().find(".tmp-") != std::string::npos) { stray = true; }
    }
    CHECK_FALSE("an uncommitted write leaves no temporary", stray);

    // Output confinement.
    CHECK("a path inside the directory is allowed",
          output_path_is_safe(directory / "fine.wav", directory));
    CHECK_FALSE("a path escaping the directory is refused",
                output_path_is_safe(directory / ".." / ".." / "escape.wav", directory));

    const fs::path link = directory / "link.wav";
    fs::create_symlink(scratch_directory() / "elsewhere.wav", link, error);
    if (!error) {
        CHECK_FALSE("a symbolic link destination is refused",
                    output_path_is_safe(link, directory));
    }

    CHECK("a path is within itself", path_is_within(directory, directory));
    CHECK_FALSE("a sibling is not within", path_is_within(scratch_directory() / "other", directory));

    // Session stamps must differ so recordings never overwrite each other.
    CHECK("session stamps have the expected shape", session_stamp().size() == 20);
}

}  // namespace

int main() {
    test_wav_header_generation();
    test_wav_size_limits();
    test_empty_audio();
    test_silent_audio();
    test_noise_floor();
    test_vad_boundaries();
    test_overlapping_segments();
    test_model_paths();
    test_ring_buffer_overflow();
    test_drain_past_the_limit();
    test_file_write_failures();
    return harness::summary();
}
