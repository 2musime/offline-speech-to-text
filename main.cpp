#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "whisper.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

enum class ErrorCategory {
    Microphone,
    Model,
    Recording,
    Transcription,
    FileSaving,
    Worker
};

const char* error_category_name(ErrorCategory category) {
    switch (category) {
        case ErrorCategory::Microphone: return "MICROPHONE";
        case ErrorCategory::Model: return "MODEL";
        case ErrorCategory::Recording: return "RECORDING";
        case ErrorCategory::Transcription: return "TRANSCRIPTION";
        case ErrorCategory::FileSaving: return "FILE_SAVING";
        case ErrorCategory::Worker: return "WORKER";
    }
    return "WORKER";
}

// Failures that end the current recording attempt.
void report_error(ErrorCategory category, const std::string& message) {
    std::cerr << "ERROR|" << error_category_name(category) << '|' << message << std::endl;
}

// Advisories that do not stop processing.
void report_warning(ErrorCategory category, const std::string& message) {
    std::cerr << "WARN|" << error_category_name(category) << '|' << message << std::endl;
}

namespace {
volatile std::sig_atomic_t shutdown_flag = 0;
}

extern "C" void request_shutdown(int) {
    shutdown_flag = 1;
}

bool shutdown_is_requested() {
    return shutdown_flag != 0;
}

// std::getline retries internally on EINTR, so a signal can never interrupt it.
// Reading the descriptor directly keeps Ctrl+C responsive while awaiting a command.
// Single-threaded use only: the leftover buffer is shared between calls.
bool read_command(std::string& command) {
    command.clear();
#ifdef __linux__
    static std::string buffer;
    while (true) {
        const std::size_t newline = buffer.find('\n');
        if (newline != std::string::npos) {
            command.assign(buffer, 0, newline);
            buffer.erase(0, newline + 1);
            return true;
        }

        char chunk[256];
        const ssize_t count = ::read(STDIN_FILENO, chunk, sizeof(chunk));
        if (count > 0) {
            buffer.append(chunk, static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            if (buffer.empty()) {
                return false;
            }
            command = buffer;
            buffer.clear();
            return true;
        }
        if (errno == EINTR) {
            if (shutdown_is_requested()) {
                return false;
            }
            continue;
        }
        return false;
    }
#else
    return static_cast<bool>(std::getline(std::cin, command));
#endif
}

void install_shutdown_handlers() {
#ifdef __linux__
    // No SA_RESTART: a blocking std::getline must return when the signal lands.
    struct sigaction action {};
    action.sa_handler = request_shutdown;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
#else
    std::signal(SIGINT, request_shutdown);
    std::signal(SIGTERM, request_shutdown);
#endif
}

class WhisperContext {
public:
    WhisperContext() = default;

    explicit WhisperContext(const std::string& model_path) {
        whisper_context_params context_params = whisper_context_default_params();
        context_ = whisper_init_from_file_with_params(model_path.c_str(), context_params);
    }

    ~WhisperContext() {
        release();
    }

    WhisperContext(const WhisperContext&) = delete;
    WhisperContext& operator=(const WhisperContext&) = delete;

    WhisperContext(WhisperContext&& other) noexcept : context_(other.context_) {
        other.context_ = nullptr;
    }

    WhisperContext& operator=(WhisperContext&& other) noexcept {
        if (this != &other) {
            release();
            context_ = other.context_;
            other.context_ = nullptr;
        }
        return *this;
    }

    whisper_context* get() const {
        return context_;
    }

    explicit operator bool() const {
        return context_ != nullptr;
    }

private:
    void release() {
        if (context_ != nullptr) {
            whisper_free(context_);
            context_ = nullptr;
        }
    }

    whisper_context* context_ = nullptr;
};

class CaptureDevice {
public:
    CaptureDevice() = default;

    ~CaptureDevice() {
        stop();
        if (initialized_) {
            ma_device_uninit(&device_);
            initialized_ = false;
        }
    }

    CaptureDevice(const CaptureDevice&) = delete;
    CaptureDevice& operator=(const CaptureDevice&) = delete;
    CaptureDevice(CaptureDevice&&) = delete;
    CaptureDevice& operator=(CaptureDevice&&) = delete;

    bool initialize(const ma_device_config& config) {
        if (initialized_ || ma_device_init(nullptr, &config, &device_) != MA_SUCCESS) {
            return false;
        }
        initialized_ = true;
        return true;
    }

    bool start() {
        if (!initialized_ || ma_device_start(&device_) != MA_SUCCESS) {
            return false;
        }
        running_ = true;
        return true;
    }

    void stop() {
        if (initialized_ && running_) {
            ma_device_stop(&device_);
            running_ = false;
        }
    }

private:
    ma_device device_{};
    bool initialized_ = false;
    bool running_ = false;
};

class ScopedThread {
public:
    ScopedThread() = default;

    explicit ScopedThread(std::thread thread) : thread_(std::move(thread)) {}

    ~ScopedThread() {
        join();
    }

    ScopedThread(const ScopedThread&) = delete;
    ScopedThread& operator=(const ScopedThread&) = delete;
    ScopedThread(ScopedThread&& other) noexcept : thread_(std::move(other.thread_)) {}

    ScopedThread& operator=(ScopedThread&& other) noexcept {
        if (this != &other) {
            join();
            thread_ = std::move(other.thread_);
        }
        return *this;
    }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    std::thread thread_;
};

struct Recording {
    std::mutex mutex;
    std::vector<std::int16_t> samples;
    std::size_t maximum_samples = 0;
    std::atomic<bool> limit_reached{false};
};

class AudioRingBuffer {
public:
    explicit AudioRingBuffer(std::size_t capacity)
        : samples_(capacity), write_position_(0), read_position_(0) {}

    std::size_t write(const std::int16_t* input, std::size_t count) {
        const std::size_t write_position = write_position_.load(std::memory_order_relaxed);
        const std::size_t read_position = read_position_.load(std::memory_order_acquire);
        const std::size_t available = samples_.size() - (write_position - read_position);
        const std::size_t written = std::min(count, available);
        for (std::size_t i = 0; i < written; ++i) {
            samples_[(write_position + i) % samples_.size()] = input[i];
        }
        write_position_.store(write_position + written, std::memory_order_release);
        return written;
    }

    std::size_t read(std::vector<std::int16_t>& output, std::size_t count) {
        const std::size_t read_position = read_position_.load(std::memory_order_relaxed);
        const std::size_t write_position = write_position_.load(std::memory_order_acquire);
        const std::size_t available = write_position - read_position;
        const std::size_t read_count = std::min(count, available);
        output.resize(read_count);
        for (std::size_t i = 0; i < read_count; ++i) {
            output[i] = samples_[(read_position + i) % samples_.size()];
        }
        read_position_.store(read_position + read_count, std::memory_order_release);
        return read_count;
    }

    std::size_t available() const {
        return write_position_.load(std::memory_order_acquire) -
            read_position_.load(std::memory_order_acquire);
    }

    void reset() {
        write_position_.store(0, std::memory_order_release);
        read_position_.store(0, std::memory_order_release);
    }

private:
    std::vector<std::int16_t> samples_;
    std::atomic<std::size_t> write_position_;
    std::atomic<std::size_t> read_position_;
};

struct StreamingCapture {
    AudioRingBuffer buffer;
    std::vector<std::int16_t> all_samples;
    std::size_t maximum_samples;
    std::atomic<std::size_t> total_samples{0};
    std::atomic<bool> recording{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> overflowed{false};

    explicit StreamingCapture(std::size_t capacity)
        : buffer(capacity), all_samples(capacity), maximum_samples(capacity) {}

    void reset() {
        buffer.reset();
        total_samples.store(0, std::memory_order_release);
        recording.store(false, std::memory_order_release);
        finished.store(false, std::memory_order_release);
        overflowed.store(false, std::memory_order_release);
        maximum_samples = all_samples.size();
    }
};

struct CaptureState {
    Recording* recording;
    StreamingCapture* streaming;
};

void capture_callback(ma_device* device, void*, const void* input, ma_uint32 frame_count) {
    if (input == nullptr) {
        return;
    }

    auto* state = static_cast<CaptureState*>(device->pUserData);
    auto* recording = state->recording;
    const auto* samples = static_cast<const std::int16_t*>(input);

    std::lock_guard<std::mutex> lock(recording->mutex);
    const std::size_t remaining = recording->maximum_samples > recording->samples.size()
        ? recording->maximum_samples - recording->samples.size()
        : 0;
    const std::size_t count = std::min<std::size_t>(frame_count, remaining);
    recording->samples.insert(recording->samples.end(), samples, samples + count);
    if (count != frame_count) {
        recording->limit_reached.store(true, std::memory_order_release);
    }
}

void streaming_capture_callback(ma_device* device, void*, const void* input, ma_uint32 frame_count) {
    if (input == nullptr) {
        return;
    }

    auto* state = static_cast<CaptureState*>(device->pUserData);
    auto* capture = state->streaming;
    const auto* samples = static_cast<const std::int16_t*>(input);
    const std::size_t offset = capture->total_samples.fetch_add(frame_count, std::memory_order_relaxed);
    const std::size_t remaining = offset < capture->maximum_samples
        ? capture->maximum_samples - offset
        : 0;
    const std::size_t count = std::min<std::size_t>(frame_count, remaining);
    if (count > 0) {
        std::copy(samples, samples + count, capture->all_samples.begin() + offset);
    } else {
        capture->overflowed.store(true, std::memory_order_release);
    }
    if (count != frame_count || capture->buffer.write(samples, count) != count) {
        capture->overflowed.store(true, std::memory_order_release);
    }
}

void capture_dispatch_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
    auto* state = static_cast<CaptureState*>(device->pUserData);
    if (state->streaming->recording.load(std::memory_order_acquire)) {
        streaming_capture_callback(device, output, input, frame_count);
    } else {
        capture_callback(device, output, input, frame_count);
    }
}

void write_little_endian(std::ofstream& file, std::uint32_t value) {
    const char bytes[] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 24) & 0xff)
    };
    file.write(bytes, sizeof(bytes));
}

bool write_wav(const char* path, const std::vector<std::int16_t>& samples, std::uint32_t sample_rate) {
    constexpr std::uint32_t channels = 1;
    constexpr std::uint32_t bits_per_sample = 16;
    if (sample_rate == 0 || channels == 0 || bits_per_sample != 16) {
        return false;
    }
    if (samples.size() > (std::numeric_limits<std::uint32_t>::max() - 36) / sizeof(std::int16_t)) {
        return false;
    }

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    const std::uint32_t data_size = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    const std::uint32_t file_size = 36 + data_size;

    file.write("RIFF", 4);
    write_little_endian(file, file_size);
    file.write("WAVEfmt ", 8);
    write_little_endian(file, 16);
    file.put(static_cast<char>(channels));
    file.put(0);
    file.put(1);
    file.put(0);
    write_little_endian(file, sample_rate);
    write_little_endian(file, sample_rate * sizeof(std::int16_t));
    file.put(static_cast<char>(channels * sizeof(std::int16_t)));
    file.put(0);
    file.put(static_cast<char>(bits_per_sample));
    file.put(0);
    file.write("data", 4);
    write_little_endian(file, data_size);
    file.write(reinterpret_cast<const char*>(samples.data()), data_size);

    return file.good();
}

struct SpeechSegment {
    std::size_t begin;
    std::size_t end;
};

struct NoiseProfile {
    float noise_rms;
    float attenuation_threshold;
};

NoiseProfile measure_noise_floor(
    const std::vector<std::int16_t>& samples,
    std::uint32_t sample_rate) {
    constexpr std::size_t frame_duration_ms = 20;
    constexpr float noise_multiplier = 1.5f;
    constexpr float minimum_threshold = 0.003f;

    const std::size_t frame_size = sample_rate * frame_duration_ms / 1000;
    const std::size_t frame_count = (samples.size() + frame_size - 1) / frame_size;
    if (frame_count == 0) {
        return {0.0f, minimum_threshold};
    }

    std::vector<float> energies;
    energies.reserve(frame_count);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const std::size_t begin = frame * frame_size;
        const std::size_t end = std::min(begin + frame_size, samples.size());
        float sum = 0.0f;
        for (std::size_t i = begin; i < end; ++i) {
            const float sample = static_cast<float>(samples[i]) / 32768.0f;
            sum += sample * sample;
        }
        energies.push_back(std::sqrt(sum / static_cast<float>(end - begin)));
    }

    std::sort(energies.begin(), energies.end());
    const float noise_rms = energies[energies.size() / 5];
    return {noise_rms, std::max(minimum_threshold, noise_rms * noise_multiplier)};
}

std::vector<std::int16_t> reduce_noise(
    const std::vector<std::int16_t>& samples,
    const NoiseProfile& profile) {
    std::vector<std::int16_t> cleaned(samples);
    for (std::int16_t& sample : cleaned) {
        const float value = static_cast<float>(sample) / 32768.0f;
        const float magnitude = std::abs(value);
        if (magnitude < profile.attenuation_threshold) {
            const float retained = magnitude / profile.attenuation_threshold;
            sample = static_cast<std::int16_t>(value * retained * 32768.0f);
        }
    }
    return cleaned;
}

std::vector<SpeechSegment> detect_speech_segments(
    const std::vector<std::int16_t>& samples,
    std::uint32_t sample_rate) {
    constexpr std::size_t frame_duration_ms = 20;
    constexpr std::size_t padding_duration_ms = 200;
    constexpr std::size_t merge_gap_ms = 300;
    constexpr float minimum_rms = 0.01f;
    constexpr float noise_multiplier = 3.0f;

    const std::size_t frame_size = sample_rate * frame_duration_ms / 1000;
    const std::size_t padding = sample_rate * padding_duration_ms / 1000;
    const std::size_t merge_gap = sample_rate * merge_gap_ms / 1000;
    const std::size_t frame_count = (samples.size() + frame_size - 1) / frame_size;
    if (frame_count == 0) {
        return {};
    }

    std::vector<float> energies(frame_count);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const std::size_t begin = frame * frame_size;
        const std::size_t end = std::min(begin + frame_size, samples.size());
        float sum = 0.0f;
        for (std::size_t i = begin; i < end; ++i) {
            const float sample = static_cast<float>(samples[i]) / 32768.0f;
            sum += sample * sample;
        }
        energies[frame] = std::sqrt(sum / static_cast<float>(end - begin));
    }

    std::vector<float> sorted_energies = energies;
    std::sort(sorted_energies.begin(), sorted_energies.end());
    const float noise_floor = sorted_energies[sorted_energies.size() / 5];
    const float threshold = std::max(minimum_rms, noise_floor * noise_multiplier);

    std::vector<SpeechSegment> segments;
    bool in_speech = false;
    std::size_t speech_begin = 0;
    std::size_t last_active_frame = 0;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        if (energies[frame] >= threshold) {
            if (!in_speech) {
                speech_begin = frame * frame_size;
                in_speech = true;
            }
            last_active_frame = frame;
        } else if (in_speech && frame > last_active_frame + merge_gap / frame_size) {
            segments.push_back({speech_begin, std::min((last_active_frame + 1) * frame_size, samples.size())});
            in_speech = false;
        }
    }
    if (in_speech) {
        segments.push_back({speech_begin, samples.size()});
    }

    for (SpeechSegment& segment : segments) {
        segment.begin = segment.begin > padding ? segment.begin - padding : 0;
        segment.end = std::min(segment.end + padding, samples.size());
    }

    std::vector<SpeechSegment> merged;
    for (const SpeechSegment& segment : segments) {
        if (!merged.empty() && segment.begin <= merged.back().end + merge_gap) {
            merged.back().end = segment.end;
        } else {
            merged.push_back(segment);
        }
    }

    constexpr std::size_t maximum_segment_duration = WHISPER_SAMPLE_RATE * 30;
    std::vector<SpeechSegment> chunks;
    for (const SpeechSegment& segment : merged) {
        for (std::size_t begin = segment.begin; begin < segment.end; begin += maximum_segment_duration) {
            chunks.push_back({begin, std::min(begin + maximum_segment_duration, segment.end)});
        }
    }
    return chunks;
}

std::vector<std::int16_t> extract_speech(
    const std::vector<std::int16_t>& samples,
    const std::vector<SpeechSegment>& segments) {
    std::vector<std::int16_t> speech;
    for (const SpeechSegment& segment : segments) {
        speech.insert(speech.end(), samples.begin() + segment.begin, samples.begin() + segment.end);
    }
    return speech;
}

struct TranscriptionResult {
    std::string text;
    long long milliseconds;
};

long long process_cpu_milliseconds() {
#ifdef __linux__
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        const auto user_us = usage.ru_utime.tv_sec * 1000000LL + usage.ru_utime.tv_usec;
        const auto system_us = usage.ru_stime.tv_sec * 1000000LL + usage.ru_stime.tv_usec;
        return (user_us + system_us) / 1000;
    }
#endif
    return 0;
}

bool run_transcription(
    whisper_context* context,
    const std::vector<std::int16_t>& samples,
    const std::vector<SpeechSegment>& segments,
    int thread_count,
    TranscriptionResult& result) {
    std::string transcription;
    const auto transcription_start = std::chrono::steady_clock::now();
    for (const SpeechSegment& segment : segments) {
        const std::vector<std::int16_t> chunk(samples.begin() + segment.begin, samples.begin() + segment.end);
        std::vector<float> audio(chunk.size());
        for (std::size_t i = 0; i < chunk.size(); ++i) {
            audio[i] = static_cast<float>(chunk[i]) / 32768.0f;
        }

        whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        params.print_progress = false;
        params.print_realtime = false;
        params.print_timestamps = false;
        params.single_segment = false;
        params.language = "en";
        params.n_threads = thread_count;

        if (whisper_full(context, params, audio.data(), audio.size()) != 0) {
            report_error(ErrorCategory::Transcription, "Whisper could not process the speech segment.");
            return false;
        }

        for (int i = 0; i < whisper_full_n_segments(context); ++i) {
            transcription += whisper_full_get_segment_text(context, i);
        }
    }

    result.text = transcription;
    result.milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - transcription_start).count();
    return true;
}

bool transcribe(
    whisper_context* context,
    const std::vector<std::int16_t>& samples,
    const std::vector<SpeechSegment>& segments,
    int thread_count) {
    TranscriptionResult result;
    if (!run_transcription(context, samples, segments, thread_count, result)) {
        return false;
    }

    std::ofstream file("transcription.txt");
    if (!file) {
        report_error(ErrorCategory::FileSaving, "Could not save transcription.txt.");
        return false;
    }
    file << result.text << '\n';

    std::cout << "\nTranscription:\n" << result.text << std::endl;
    std::cout << "Saved transcription.txt" << std::endl;
    std::cout << "Whisper processing time: " << result.milliseconds << " ms" << std::endl;
    return true;
}

void streaming_worker(
    StreamingCapture& capture,
    whisper_context* context,
    int thread_count,
    std::uint32_t sample_rate) {
    constexpr std::size_t window_seconds = 5;
    constexpr std::size_t stride_seconds = 4;
    const std::size_t window_size = sample_rate * window_seconds;
    const std::size_t stride_size = sample_rate * stride_seconds;
    std::vector<std::int16_t> pending;
    std::vector<std::int16_t> incoming;
    pending.reserve(window_size + stride_size);
    const long long cpu_start = process_cpu_milliseconds();

    while (!shutdown_is_requested() &&
           (!capture.finished.load(std::memory_order_acquire) || capture.buffer.available() > 0)) {
        if (capture.buffer.read(incoming, sample_rate / 10) > 0) {
            pending.insert(pending.end(), incoming.begin(), incoming.end());
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        while (pending.size() >= window_size) {
            const auto start = std::chrono::steady_clock::now();
            std::vector<std::int16_t> window(pending.begin(), pending.begin() + window_size);
            TranscriptionResult result;
            const std::vector<SpeechSegment> range{{0, window.size()}};
            if (!run_transcription(context, window, range, thread_count, result)) {
                report_error(ErrorCategory::Transcription, "Streaming transcription failed for an audio window.");
                return;
            }
            const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            std::cout << "\nPartial (window ending at " << window_seconds
                      << "s, latency " << latency << " ms):\n"
                      << result.text << std::endl;
            pending.erase(pending.begin(), pending.begin() + stride_size);
        }
    }

    if (capture.overflowed.load(std::memory_order_acquire)) {
        report_warning(ErrorCategory::Recording, "Streaming buffer overflowed; audio was dropped.");
    }
    std::cout << "Streaming worker CPU time: "
              << process_cpu_milliseconds() - cpu_start << " ms" << std::endl;
}

int default_thread_count() {
    const unsigned int hardware_threads = std::thread::hardware_concurrency();
    if (hardware_threads <= 1) {
        return 1;
    }
    return static_cast<int>(hardware_threads - 1);
}

long peak_memory_kb() {
#ifdef __linux__
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return usage.ru_maxrss;
    }
#endif
    return 0;
}

bool parse_options(
    int argc,
    char** argv,
    int& thread_count,
    bool& compare_mode,
    bool& streaming_mode,
    int& duration_seconds,
    std::vector<std::string>& model_paths) {
    thread_count = default_thread_count();
    compare_mode = false;
    streaming_mode = false;
    duration_seconds = 15;
    model_paths.clear();

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--threads") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --threads." << std::endl;
                return false;
            }
            try {
                const long parsed = std::stol(argv[++i]);
                if (parsed < 1 || parsed > std::numeric_limits<int>::max()) {
                    throw std::out_of_range("thread count");
                }
                thread_count = static_cast<int>(parsed);
            } catch (const std::exception&) {
                std::cerr << "Thread count must be a positive integer." << std::endl;
                return false;
            }
        } else if (argument == "--compare") {
            compare_mode = true;
        } else if (argument == "--stream") {
            streaming_mode = true;
        } else if (argument == "--duration") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --duration." << std::endl;
                return false;
            }
            try {
                duration_seconds = std::stoi(argv[++i]);
            } catch (const std::exception&) {
                std::cerr << "Duration must be 15, 45, or 60 seconds." << std::endl;
                return false;
            }
            if (duration_seconds != 15 && duration_seconds != 45 && duration_seconds != 60) {
                std::cerr << "Duration must be 15, 45, or 60 seconds." << std::endl;
                return false;
            }
        } else if (argument == "--help" || argument == "-h") {
            std::cout << "Usage:\n"
                      << "  ./build/audio_to_text MODEL_PATH [--threads N] [--duration 15|45|60] [--stream]\n"
                      << "  ./build/audio_to_text --compare MODEL_PATH MODEL_PATH ... [--threads N]"
                      << std::endl;
            return false;
        } else if (!argument.empty() && argument[0] == '-') {
            std::cerr << "Unknown option: " << argument << std::endl;
            return false;
        } else {
            model_paths.push_back(argument);
        }
    }

    if (model_paths.empty() && !compare_mode) {
        model_paths.emplace_back("models/ggml-base.en.bin");
    }
    if (!compare_mode && model_paths.size() > 1) {
        std::cerr << "Use --compare to test multiple models." << std::endl;
        return false;
    }
    if (compare_mode && model_paths.size() < 2) {
        std::cerr << "--compare requires at least two model paths." << std::endl;
        return false;
    }
    return true;
}

bool compare_models(
    const std::vector<std::string>& model_paths,
    const std::vector<std::int16_t>& original,
    const std::vector<std::int16_t>& cleaned,
    int thread_count) {
    std::cout << "\nModel comparison (same audio and VAD segments):\n";
    const std::vector<SpeechSegment> speech_range{{0, original.size()}};
    for (const std::string& model_path : model_paths) {
        WhisperContext context(model_path);
        if (!context) {
            report_error(ErrorCategory::Model, "Could not load Whisper model: " + model_path);
            return false;
        }

        TranscriptionResult original_result;
        TranscriptionResult cleaned_result;
        const bool success = run_transcription(
                context.get(), original, speech_range, thread_count, original_result) &&
            run_transcription(context.get(), cleaned, speech_range, thread_count, cleaned_result);
        const long memory_kb = peak_memory_kb();
        if (!success) {
            return false;
        }

        std::error_code error;
        const auto model_size = std::filesystem::file_size(model_path, error);
        const double model_size_mb = error ? 0.0 : model_size / (1024.0 * 1024.0);
        std::cout << "\nModel: " << model_path
                  << "\n  Size: " << model_size_mb << " MB"
                  << "\n  Original processing time: " << original_result.milliseconds << " ms"
                  << "\n  Cleaned processing time: " << cleaned_result.milliseconds << " ms"
                  << "\n  Peak memory: " << memory_kb << " KB"
                  << "\n  Original transcription: " << original_result.text
                  << "\n  Cleaned transcription: " << cleaned_result.text << std::endl;
    }
    return true;
}

int run(int argc, char** argv) {
    constexpr ma_uint32 sample_rate = WHISPER_SAMPLE_RATE;
    Recording recording;

    int thread_count = 0;
    bool compare_mode = false;
    bool streaming_mode = false;
    int duration_seconds = 15;
    std::vector<std::string> model_paths;
    if (!parse_options(argc, argv, thread_count, compare_mode, streaming_mode, duration_seconds, model_paths)) {
        return argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") ? 0 : 1;
    }

    install_shutdown_handlers();

    const unsigned int hardware_threads = std::thread::hardware_concurrency();
    std::cout << "CPU threads detected: " << (hardware_threads == 0 ? 1 : hardware_threads)
              << ", Whisper threads: " << thread_count
              << ", recording limit: " << duration_seconds << " seconds" << std::endl;

    if (compare_mode && streaming_mode) {
        std::cerr << "--stream cannot be combined with --compare." << std::endl;
        return 1;
    }

    WhisperContext context;
    if (!compare_mode) {
        context = WhisperContext(model_paths[0]);
        if (!context) {
            report_error(ErrorCategory::Model, "Could not load Whisper model: " + model_paths[0]);
            return 1;
        }
    }

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_s16;
    config.capture.channels = 1;
    config.sampleRate = sample_rate;
    const std::size_t maximum_samples = static_cast<std::size_t>(sample_rate) * duration_seconds;
    recording.maximum_samples = maximum_samples;
    StreamingCapture streaming_capture(maximum_samples);
    CaptureState capture_state{&recording, &streaming_capture};
    config.dataCallback = capture_dispatch_callback;
    config.pUserData = &capture_state;

    CaptureDevice device;
    if (!device.initialize(config)) {
        report_error(ErrorCategory::Microphone, "Could not open the microphone.");
        return 1;
    }

    while (!shutdown_is_requested()) {
        std::cout << "Press Enter to start recording, or type q to quit." << std::endl;
        std::string command;
        const bool have_command = read_command(command);
        if (shutdown_is_requested()) {
            break;
        }
        if (!have_command || command == "q" || command == "Q") {
            break;
        }

        {
            std::lock_guard<std::mutex> lock(recording.mutex);
            recording.samples.clear();
            recording.samples.reserve(recording.maximum_samples);
        }
        recording.limit_reached.store(false, std::memory_order_release);

        if (streaming_mode) {
            streaming_capture.reset();
            streaming_capture.recording.store(true, std::memory_order_release);
            if (!device.start()) {
                streaming_capture.recording.store(false, std::memory_order_release);
                report_error(ErrorCategory::Microphone, "Could not start recording.");
                break;
            }

            ScopedThread worker(std::thread(
                streaming_worker, std::ref(streaming_capture), context.get(), thread_count, sample_rate));
            std::cout << "Streaming recording... Press Enter to stop." << std::endl;
            read_command(command);
            streaming_capture.recording.store(false, std::memory_order_release);
            device.stop();
            streaming_capture.finished.store(true, std::memory_order_release);
            worker.join();

            const std::size_t sample_count = std::min(
                streaming_capture.total_samples.load(std::memory_order_acquire),
                streaming_capture.all_samples.size());
            recording.samples.assign(
                streaming_capture.all_samples.begin(),
                streaming_capture.all_samples.begin() + sample_count);
            if (streaming_capture.overflowed.load(std::memory_order_acquire)) {
                report_warning(ErrorCategory::Recording,
                    "Recording reached its limit or the streaming buffer overflowed; audio was truncated.");
            }
        } else {
            if (!device.start()) {
                report_error(ErrorCategory::Microphone, "Could not start recording.");
                break;
            }

            std::cout << "Recording... Press Enter to stop." << std::endl;
            read_command(command);
            device.stop();
        }

        if (shutdown_is_requested()) {
            break;
        }

        if (recording.samples.empty()) {
            report_error(ErrorCategory::Recording, "No audio was captured. Check the microphone and try again.");
            continue;
        }
        if (recording.limit_reached.load(std::memory_order_acquire)) {
            report_warning(ErrorCategory::Recording,
                "Recording reached the " + std::to_string(duration_seconds) +
                " second limit; extra audio was discarded.");
        }

        if (!write_wav("recording.wav", recording.samples, sample_rate)) {
            report_error(ErrorCategory::FileSaving, "Could not save recording.wav.");
            break;
        }

        std::cout << "Saved recording.wav ("
                  << recording.samples.size() / static_cast<double>(sample_rate)
                  << " seconds)." << std::endl;

        const NoiseProfile noise_profile = measure_noise_floor(recording.samples, sample_rate);
        const std::vector<std::int16_t> cleaned_samples = reduce_noise(recording.samples, noise_profile);
        if (!write_wav("cleaned.wav", cleaned_samples, sample_rate)) {
            report_error(ErrorCategory::FileSaving, "Could not save cleaned.wav.");
            break;
        }

        const float noise_db = noise_profile.noise_rms > 0.0f
            ? 20.0f * std::log10(noise_profile.noise_rms)
            : -std::numeric_limits<float>::infinity();
        std::cout << "Measured noise floor: " << noise_profile.noise_rms
                  << " RMS (" << noise_db << " dBFS), attenuation threshold: "
                  << noise_profile.attenuation_threshold << std::endl;
        std::cout << "Saved cleaned.wav (speech length preserved)." << std::endl;

        const auto vad_start = std::chrono::steady_clock::now();
        const std::vector<SpeechSegment> speech_segments = detect_speech_segments(recording.samples, sample_rate);
        const auto vad_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - vad_start).count();

        if (speech_segments.empty()) {
            report_error(ErrorCategory::Recording, "No speech detected. Try speaking closer to the microphone.");
            continue;
        }

        const std::vector<std::int16_t> speech = extract_speech(recording.samples, speech_segments);
        const std::vector<std::int16_t> cleaned_speech = extract_speech(cleaned_samples, speech_segments);
        if (!write_wav("speech.wav", speech, sample_rate)) {
            report_error(ErrorCategory::FileSaving, "Could not save speech.wav.");
            break;
        }

        const double raw_seconds = recording.samples.size() / static_cast<double>(sample_rate);
        const double speech_seconds = speech.size() / static_cast<double>(sample_rate);
        const double reduction = raw_seconds > 0.0 ? (1.0 - speech_seconds / raw_seconds) * 100.0 : 0.0;
        std::cout << "Speech detected: " << speech_seconds << " seconds in "
                  << speech_segments.size() << " segment(s)." << std::endl;
        std::cout << "Saved speech.wav. Removed " << reduction << "% of recorded audio."
                  << " VAD time: " << vad_ms << " ms" << std::endl;

        if (compare_mode) {
            if (!compare_models(model_paths, speech, cleaned_speech, thread_count)) {
                break;
            }
        } else if (context) {
            const std::vector<SpeechSegment> speech_range{{0, speech.size()}};
            if (!transcribe(context.get(), speech, speech_range, thread_count)) {
                break;
            }
        }
    }

    if (shutdown_is_requested()) {
        std::cout << "\nShutting down." << std::endl;
    }
    return 0;
}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::bad_alloc&) {
        report_error(ErrorCategory::Worker, "Ran out of memory.");
        return 1;
    } catch (const std::exception& error) {
        report_error(ErrorCategory::Worker, std::string("Unexpected failure: ") + error.what());
        return 1;
    } catch (...) {
        report_error(ErrorCategory::Worker, "Unexpected failure.");
        return 1;
    }
}
