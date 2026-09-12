#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "whisper.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <iomanip>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
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

// Turns a miniaudio result into something a user can act on.
std::string describe_device_error(ma_result result) {
    switch (result) {
        case MA_ACCESS_DENIED:
            return "Access to the microphone was denied. Grant this application "
                "permission to use the microphone and try again.";
        case MA_NO_DEVICE:
            return "No capture device is available. Connect a microphone and try again.";
        case MA_DEVICE_NOT_INITIALIZED:
            return "The capture device is no longer initialised. It may have been disconnected.";
        case MA_DEVICE_NOT_STARTED:
            return "The capture device did not start.";
        case MA_BUSY:
            return "The microphone is in use by another application.";
        default:
            return std::string("The capture device could not be used (") +
                ma_result_description(result) + ").";
    }
}

// Devices are enumerated and opened from the same context, so a chosen device
// identifier stays valid between listing and opening.
class AudioContext {
public:
    AudioContext() = default;

    ~AudioContext() {
        if (initialized_) {
            ma_context_uninit(&context_);
            initialized_ = false;
        }
    }

    AudioContext(const AudioContext&) = delete;
    AudioContext& operator=(const AudioContext&) = delete;
    AudioContext(AudioContext&&) = delete;
    AudioContext& operator=(AudioContext&&) = delete;

    ma_result initialize() {
        const ma_result result = ma_context_init(nullptr, 0, nullptr, &context_);
        initialized_ = result == MA_SUCCESS;
        return result;
    }

    ma_result capture_devices(ma_device_info** infos, ma_uint32* count) {
        if (!initialized_) {
            return MA_DEVICE_NOT_INITIALIZED;
        }
        return ma_context_get_devices(&context_, nullptr, nullptr, infos, count);
    }

    ma_context* get() {
        return initialized_ ? &context_ : nullptr;
    }

private:
    ma_context context_{};
    bool initialized_ = false;
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

    ma_result initialize(ma_context* context, const ma_device_config& config) {
        if (initialized_) {
            return MA_INVALID_OPERATION;
        }
        const ma_result result = ma_device_init(context, &config, &device_);
        initialized_ = result == MA_SUCCESS;
        return result;
    }

    ma_result start() {
        if (!initialized_) {
            return MA_DEVICE_NOT_INITIALIZED;
        }
        const ma_result result = ma_device_start(&device_);
        running_ = result == MA_SUCCESS;
        return result;
    }

    void stop() {
        if (initialized_ && running_) {
            ma_device_stop(&device_);
            running_ = false;
        }
    }

    // Empty when the backend does not report a name.
    std::string name() const {
        if (!initialized_) {
            return {};
        }
        return std::string(device_.capture.name);
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

namespace fs = std::filesystem;

// Files are written only inside a directory the application owns.
fs::path application_data_directory() {
    const char* data_home = std::getenv("XDG_DATA_HOME");
    if (data_home != nullptr && data_home[0] == '/') {
        return fs::path(data_home) / "audio-to-text";
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] == '/') {
        return fs::path(home) / ".local" / "share" / "audio-to-text";
    }
    return fs::current_path() / ".audio-to-text";
}

// Creates the directory if needed and restricts it to the owner.
bool ensure_private_directory(const fs::path& directory) {
    std::error_code error;
    fs::create_directories(directory, error);
    if (error && !fs::is_directory(directory)) {
        return false;
    }
    if (!fs::is_directory(directory)) {
        return false;
    }
#ifdef __linux__
    if (::chmod(directory.c_str(), S_IRWXU) != 0) {
        return false;
    }
#endif
    return true;
}

// True when candidate resolves to root itself or something beneath it.
bool path_is_within(const fs::path& candidate, const fs::path& root) {
    std::error_code error;
    const fs::path resolved_candidate = fs::weakly_canonical(candidate, error);
    if (error) {
        return false;
    }
    const fs::path resolved_root = fs::weakly_canonical(root, error);
    if (error) {
        return false;
    }

    auto candidate_part = resolved_candidate.begin();
    auto root_part = resolved_root.begin();
    for (; root_part != resolved_root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == resolved_candidate.end() || *candidate_part != *root_part) {
            return false;
        }
    }
    return true;
}

// Groups the artefacts of one recording: 20260912-154233-7f3a.
std::string session_stamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm parts{};
#ifdef __linux__
    localtime_r(&seconds, &parts);
#else
    parts = *std::localtime(&seconds);
#endif

    std::random_device device;
    std::ostringstream stream;
    stream << std::put_time(&parts, "%Y%m%d-%H%M%S") << '-'
           << std::hex << std::setw(4) << std::setfill('0') << (device() & 0xffff);
    return stream.str();
}

// Writes through a temporary file and renames, so readers never observe a
// partial file. Owner-only permissions; refuses to follow a symbolic link.
class AtomicFile {
public:
    explicit AtomicFile(fs::path target)
        : target_(std::move(target)),
          temporary_(target_.string() + ".tmp-" + std::to_string(::getpid())) {}

    ~AtomicFile() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
        if (!committed_) {
            std::error_code error;
            fs::remove(temporary_, error);
        }
    }

    AtomicFile(const AtomicFile&) = delete;
    AtomicFile& operator=(const AtomicFile&) = delete;
    AtomicFile(AtomicFile&&) = delete;
    AtomicFile& operator=(AtomicFile&&) = delete;

    bool open() {
        std::error_code error;
        fs::remove(temporary_, error);
        // O_EXCL with O_CREAT never follows a link at the final component.
        descriptor_ = ::open(
            temporary_.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
            S_IRUSR | S_IWUSR);
        return descriptor_ >= 0;
    }

    bool write(const void* data, std::size_t size) {
        const char* bytes = static_cast<const char*>(data);
        std::size_t written = 0;
        while (written < size) {
            const ssize_t count = ::write(descriptor_, bytes + written, size - written);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            written += static_cast<std::size_t>(count);
        }
        return true;
    }

    bool commit() {
        if (descriptor_ < 0 || ::fsync(descriptor_) != 0) {
            return false;
        }
        const int closed = ::close(descriptor_);
        descriptor_ = -1;
        if (closed != 0 || ::rename(temporary_.c_str(), target_.c_str()) != 0) {
            return false;
        }
        sync_parent_directory();
        committed_ = true;
        return true;
    }

private:
    // Makes the rename itself durable, not just the file contents.
    void sync_parent_directory() const {
        const int directory = ::open(
            target_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory >= 0) {
            ::fsync(directory);
            ::close(directory);
        }
    }

    fs::path target_;
    fs::path temporary_;
    int descriptor_ = -1;
    bool committed_ = false;
};

// Models must resolve inside an approved root; symbolic links are followed but
// the destination still has to land inside one of those roots.
bool resolve_model_path(
    const std::string& requested,
    const std::vector<fs::path>& roots,
    fs::path& resolved,
    std::string& reason) {
    std::error_code error;
    const fs::path canonical = fs::canonical(requested, error);
    if (error) {
        reason = "Model file does not exist or is not readable: " + requested;
        return false;
    }
    if (!fs::is_regular_file(canonical, error) || error) {
        reason = "Model path is not a regular file: " + requested;
        return false;
    }

    for (const fs::path& root : roots) {
        if (fs::is_directory(root, error) && path_is_within(canonical, root)) {
            resolved = canonical;
            return true;
        }
    }

    std::string allowed;
    for (const fs::path& root : roots) {
        allowed += (allowed.empty() ? "" : ", ") + root.string();
    }
    reason = "Model is outside the approved directories (" + allowed +
        "): " + requested + ". Pass --model-dir to approve another directory.";
    return false;
}

// Defence in depth: every output must stay inside the data directory, and an
// existing symbolic link at the destination is never overwritten.
bool output_path_is_safe(const fs::path& path, const fs::path& root) {
    if (!path_is_within(path, root)) {
        report_error(ErrorCategory::FileSaving,
            "Refusing to write outside the data directory: " + path.string());
        return false;
    }
    std::error_code error;
    if (fs::is_symlink(fs::symlink_status(path, error))) {
        report_error(ErrorCategory::FileSaving,
            "Refusing to overwrite a symbolic link: " + path.string());
        return false;
    }
    return true;
}

struct ModelMetadata {
    fs::path path;
    std::uintmax_t size_bytes = 0;
    std::string variant = "unknown";
    bool multilingual = false;
    std::int32_t audio_layers = 0;
    std::int32_t vocabulary_size = 0;
    std::int32_t mel_bands = 0;
    std::int32_t quantisation = 0;
};

// A ggml Whisper model begins with a magic number followed by eleven 32-bit
// hyper-parameters. Field order and the layer-count mapping were taken from
// third_party/whisper.cpp (src/whisper.cpp, whisper_model_load).
bool inspect_model(const fs::path& path, ModelMetadata& metadata, std::string& reason) {
    constexpr std::uint32_t ggml_magic = 0x67676d6c;
    constexpr std::size_t header_size = 48;
    // The smallest published Whisper model is tens of megabytes; anything under
    // a megabyte is truncated rather than merely unusual.
    constexpr std::uintmax_t smallest_plausible_model = 1024 * 1024;

    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error) {
        reason = "Could not determine the size of the model: " + path.string();
        return false;
    }
    if (size < smallest_plausible_model) {
        reason = "Model file is far too small to be a Whisper model and is probably "
            "truncated or incomplete: " + path.string();
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        reason = "Model file exists but could not be opened for reading: " + path.string();
        return false;
    }

    char header[header_size];
    if (!file.read(header, header_size)) {
        reason = "Could not read the model header: " + path.string();
        return false;
    }

    const auto field = [&header](std::size_t index) {
        std::uint32_t value = 0;
        std::memcpy(&value, header + index * sizeof(value), sizeof(value));
        return static_cast<std::int32_t>(value);
    };

    if (static_cast<std::uint32_t>(field(0)) != ggml_magic) {
        reason = "Not a ggml Whisper model; the file does not start with the ggml "
            "magic number: " + path.string();
        return false;
    }

    metadata.path = path;
    metadata.size_bytes = size;
    metadata.vocabulary_size = field(1);
    metadata.audio_layers = field(5);
    metadata.mel_bands = field(10);
    metadata.quantisation = field(11);

    if (metadata.vocabulary_size < 1000 || metadata.vocabulary_size > 1000000) {
        reason = "Model header is corrupted; the vocabulary size (" +
            std::to_string(metadata.vocabulary_size) + ") is implausible: " + path.string();
        return false;
    }
    if (metadata.mel_bands != 80 && metadata.mel_bands != 128) {
        reason = "Unsupported mel band count (" + std::to_string(metadata.mel_bands) +
            "); this build expects 80 or 128: " + path.string();
        return false;
    }

    switch (metadata.audio_layers) {
        case 4: metadata.variant = "tiny"; break;
        case 6: metadata.variant = "base"; break;
        case 12: metadata.variant = "small"; break;
        case 24: metadata.variant = "medium"; break;
        case 32: metadata.variant = "large"; break;
        default:
            reason = "Unrecognised Whisper model geometry (" +
                std::to_string(metadata.audio_layers) +
                " audio layers); the file may be corrupted or built for a different "
                "runtime: " + path.string();
            return false;
    }

    metadata.multilingual = metadata.vocabulary_size >= 51865;
    return true;
}

// The file is only ever opened for reading. These checks flag a model whose
// name or permissions disagree with what the header actually says.
void report_model_observations(const ModelMetadata& metadata) {
    std::error_code error;
    const fs::perms permissions = fs::status(metadata.path, error).permissions();
    constexpr fs::perms executable =
        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
    if (!error && (permissions & executable) != fs::perms::none) {
        report_warning(ErrorCategory::Model,
            "Model file is marked executable. It is only ever read as data, never run: " +
            metadata.path.string());
    }

    const std::string filename = metadata.path.filename().string();
    if (filename.find(metadata.variant) == std::string::npos) {
        report_warning(ErrorCategory::Model,
            "File name does not mention the model type in its header (" +
            metadata.variant + "): " + filename);
    }

    const bool name_claims_english = filename.find(".en.") != std::string::npos;
    if (name_claims_english && metadata.multilingual) {
        report_warning(ErrorCategory::Model,
            "File name claims an English-only model but the header is multilingual: " + filename);
    }
    if (!name_claims_english && !metadata.multilingual) {
        report_warning(ErrorCategory::Model,
            "Header reports an English-only model but the file name does not say so: " + filename);
    }
}

void report_model(const ModelMetadata& metadata) {
    std::cout << "MODEL|" << metadata.variant << '|'
              << (metadata.multilingual ? "multilingual" : "english") << '|'
              << metadata.size_bytes << '|' << metadata.path.string() << std::endl;
    std::cout << "Model: " << metadata.variant << ", "
              << (metadata.multilingual ? "multilingual" : "English-only") << ", "
              << metadata.size_bytes / (1024.0 * 1024.0) << " MB, "
              << metadata.audio_layers << " audio layers, "
              << metadata.mel_bands << " mel bands" << std::endl;
}

void report_saved(const char* kind, const fs::path& path) {
    std::cout << "SAVED|" << kind << '|' << path.string() << std::endl;
}

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

// One bounded sink for every recording mode. The audio callback does nothing
// but copy into the ring buffer and bump counters: no lock, no allocation, no
// system call, no branch on recording mode.
struct AudioCapture {
    AudioRingBuffer buffer;
    std::atomic<std::size_t> captured_frames{0};
    std::atomic<std::size_t> dropped_frames{0};
    std::atomic<bool> device_lost{false};
    std::atomic<bool> expected_stop{false};
    std::atomic<bool> finished{false};

    explicit AudioCapture(std::size_t capacity) : buffer(capacity) {}

    void reset() {
        buffer.reset();
        captured_frames.store(0, std::memory_order_release);
        dropped_frames.store(0, std::memory_order_release);
        device_lost.store(false, std::memory_order_release);
        expected_stop.store(false, std::memory_order_release);
        finished.store(false, std::memory_order_release);
    }

    double dropped_seconds(std::uint32_t sample_rate) const {
        return static_cast<double>(dropped_frames.load(std::memory_order_acquire)) / sample_rate;
    }
};

void capture_callback(ma_device* device, void*, const void* input, ma_uint32 frame_count) {
    if (input == nullptr) {
        return;
    }

    auto* capture = static_cast<AudioCapture*>(device->pUserData);
    const auto* samples = static_cast<const std::int16_t*>(input);
    const std::size_t written = capture->buffer.write(samples, frame_count);
    capture->captured_frames.fetch_add(written, std::memory_order_relaxed);
    if (written != frame_count) {
        capture->dropped_frames.fetch_add(frame_count - written, std::memory_order_relaxed);
    }
}

// A reroute, a system interruption, or a stop we did not ask for all mean the
// capture stream can no longer be trusted.
void capture_notification_callback(const ma_device_notification* notification) {
    if (notification == nullptr || notification->pDevice == nullptr) {
        return;
    }
    auto* capture = static_cast<AudioCapture*>(notification->pDevice->pUserData);
    if (capture == nullptr) {
        return;
    }

    switch (notification->type) {
        case ma_device_notification_type_stopped:
            // miniaudio also raises this for our own ma_device_stop call.
            if (!capture->expected_stop.load(std::memory_order_acquire)) {
                capture->device_lost.store(true, std::memory_order_release);
            }
            break;
        case ma_device_notification_type_rerouted:
        case ma_device_notification_type_interruption_began:
            capture->device_lost.store(true, std::memory_order_release);
            break;
        default:
            break;
    }
}

// Moves buffered audio into `samples`, never past `limit`. Runs on a normal
// thread; the reserved capacity means the insert does not allocate.
std::size_t drain_capture(
    AudioCapture& capture,
    std::vector<std::int16_t>& samples,
    std::vector<std::int16_t>& scratch,
    std::size_t limit) {
    constexpr std::size_t chunk = 4096;
    std::size_t moved = 0;
    while (samples.size() < limit) {
        const std::size_t wanted = std::min(limit - samples.size(), chunk);
        const std::size_t count = capture.buffer.read(scratch, wanted);
        if (count == 0) {
            break;
        }
        samples.insert(samples.end(), scratch.begin(), scratch.begin() + count);
        moved += count;
    }
    return moved;
}

void append_little_endian(std::vector<char>& buffer, std::uint32_t value) {
    buffer.push_back(static_cast<char>(value & 0xff));
    buffer.push_back(static_cast<char>((value >> 8) & 0xff));
    buffer.push_back(static_cast<char>((value >> 16) & 0xff));
    buffer.push_back(static_cast<char>((value >> 24) & 0xff));
}

bool write_wav(const fs::path& path, const std::vector<std::int16_t>& samples, std::uint32_t sample_rate) {
    constexpr std::uint32_t channels = 1;
    constexpr std::uint32_t bits_per_sample = 16;
    constexpr std::uint32_t pcm_format = 1;
    if (sample_rate == 0) {
        return false;
    }
    if (samples.size() > (std::numeric_limits<std::uint32_t>::max() - 36) / sizeof(std::int16_t)) {
        return false;
    }

    const std::uint32_t data_size = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    const std::uint32_t block_align = channels * bits_per_sample / 8;

    std::vector<char> header;
    header.reserve(44);
    const char riff_tag[] = {'R', 'I', 'F', 'F'};
    header.insert(header.end(), std::begin(riff_tag), std::end(riff_tag));
    append_little_endian(header, 36 + data_size);
    const char wave_tag[] = {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
    header.insert(header.end(), std::begin(wave_tag), std::end(wave_tag));
    append_little_endian(header, 16);
    // Each pair below packs two 16-bit fields, low half first.
    append_little_endian(header, (channels << 16) | pcm_format);
    append_little_endian(header, sample_rate);
    append_little_endian(header, sample_rate * block_align);
    append_little_endian(header, (bits_per_sample << 16) | block_align);
    const char data_tag[] = {'d', 'a', 't', 'a'};
    header.insert(header.end(), std::begin(data_tag), std::end(data_tag));
    append_little_endian(header, data_size);

    AtomicFile file(path);
    if (!file.open() || !file.write(header.data(), header.size())) {
        return false;
    }
    if (data_size > 0 && !file.write(samples.data(), data_size)) {
        return false;
    }
    return file.commit();
}

// Minimal RIFF/WAVE reader for benchmark input. Deliberately strict: the
// recorder's own format only, so a mismatch is reported instead of silently
// producing a meaningless measurement.
bool read_wav(
    const fs::path& path,
    std::vector<std::int16_t>& samples,
    std::uint32_t expected_sample_rate,
    std::string& reason) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        reason = "Could not open the audio file: " + path.string();
        return false;
    }

    char riff[12];
    if (!file.read(riff, sizeof(riff)) ||
        std::memcmp(riff, "RIFF", 4) != 0 ||
        std::memcmp(riff + 8, "WAVE", 4) != 0) {
        reason = "Not a RIFF/WAVE file: " + path.string();
        return false;
    }

    const auto read_u32 = [&file](std::uint32_t& value) {
        char bytes[4];
        if (!file.read(bytes, sizeof(bytes))) {
            return false;
        }
        std::memcpy(&value, bytes, sizeof(value));
        return true;
    };
    const auto read_u16 = [&file](std::uint16_t& value) {
        char bytes[2];
        if (!file.read(bytes, sizeof(bytes))) {
            return false;
        }
        std::memcpy(&value, bytes, sizeof(value));
        return true;
    };

    bool have_format = false;
    std::uint16_t channels = 0;
    std::uint16_t bits_per_sample = 0;
    std::uint32_t sample_rate = 0;

    while (true) {
        char id[4];
        std::uint32_t chunk_size = 0;
        if (!file.read(id, sizeof(id)) || !read_u32(chunk_size)) {
            break;
        }

        if (std::memcmp(id, "fmt ", 4) == 0 && chunk_size >= 16) {
            std::uint16_t format = 0;
            std::uint32_t byte_rate = 0;
            std::uint16_t block_align = 0;
            if (!read_u16(format) || !read_u16(channels) || !read_u32(sample_rate) ||
                !read_u32(byte_rate) || !read_u16(block_align) || !read_u16(bits_per_sample)) {
                reason = "Truncated format chunk: " + path.string();
                return false;
            }
            if (format != 1) {
                reason = "Only uncompressed PCM is supported: " + path.string();
                return false;
            }
            have_format = true;
            file.seekg(chunk_size - 16, std::ios::cur);
        } else if (std::memcmp(id, "data", 4) == 0) {
            if (!have_format) {
                reason = "Data chunk appears before the format chunk: " + path.string();
                return false;
            }
            if (channels != 1 || bits_per_sample != 16 || sample_rate != expected_sample_rate) {
                reason = "Expected mono 16-bit PCM at " + std::to_string(expected_sample_rate) +
                    " Hz but found " + std::to_string(channels) + " channel(s), " +
                    std::to_string(bits_per_sample) + "-bit at " + std::to_string(sample_rate) +
                    " Hz: " + path.string();
                return false;
            }

            samples.resize(chunk_size / sizeof(std::int16_t));
            if (samples.empty()) {
                reason = "The audio file contains no samples: " + path.string();
                return false;
            }
            if (!file.read(reinterpret_cast<char*>(samples.data()),
                           static_cast<std::streamsize>(chunk_size))) {
                reason = "Truncated audio data: " + path.string();
                return false;
            }
            return true;
        } else {
            file.seekg(chunk_size + (chunk_size & 1), std::ios::cur);
        }
    }

    reason = "No data chunk found: " + path.string();
    return false;
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

// Holds the float conversion buffer across calls. Whisper wants float samples
// and the recorder produces 16-bit ones, so this conversion happens for every
// segment of every recording; without reuse each one allocated twice.
struct TranscriptionWorkspace {
    std::vector<float> audio;
};

long peak_memory_kb() {
#ifdef __linux__
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return usage.ru_maxrss;
    }
#endif
    return 0;
}

bool run_transcription(
    whisper_context* context,
    const std::vector<std::int16_t>& samples,
    const std::vector<SpeechSegment>& segments,
    int thread_count,
    TranscriptionWorkspace& workspace,
    TranscriptionResult& result) {
    std::string transcription;
    const auto transcription_start = std::chrono::steady_clock::now();
    for (const SpeechSegment& segment : segments) {
        // Convert straight out of the source range: no intermediate copy, and
        // resize keeps whatever capacity the previous segment left behind.
        const std::size_t count = segment.end - segment.begin;
        workspace.audio.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            workspace.audio[i] = static_cast<float>(samples[segment.begin + i]) / 32768.0f;
        }

        whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        params.print_progress = false;
        params.print_realtime = false;
        params.print_timestamps = false;
        params.single_segment = false;
        params.language = "en";
        params.n_threads = thread_count;

        if (whisper_full(context, params, workspace.audio.data(), workspace.audio.size()) != 0) {
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
    int thread_count,
    TranscriptionWorkspace& workspace,
    const fs::path& output_path) {
    TranscriptionResult result;
    if (!run_transcription(context, samples, segments, thread_count, workspace, result)) {
        return false;
    }

    const std::string text = result.text + "\n";
    AtomicFile file(output_path);
    if (!file.open() || !file.write(text.data(), text.size()) || !file.commit()) {
        report_error(ErrorCategory::FileSaving,
            "Could not save the transcription to " + output_path.string() + ".");
        return false;
    }

    std::cout << "\nTranscription:\n" << result.text << std::endl;
    report_saved("TRANSCRIPT", output_path);
    std::cout << "Whisper processing time: " << result.milliseconds << " ms" << std::endl;
    return true;
}

void streaming_worker(
    AudioCapture& capture,
    std::vector<std::int16_t>& all_samples,
    std::size_t maximum_samples,
    whisper_context* context,
    int thread_count,
    std::uint32_t sample_rate) {
    constexpr std::size_t window_seconds = 5;
    constexpr std::size_t stride_seconds = 4;
    const std::size_t window_size = sample_rate * window_seconds;
    const std::size_t stride_size = sample_rate * stride_seconds;
    std::vector<std::int16_t> pending;
    std::vector<std::int16_t> scratch;
    TranscriptionWorkspace workspace;
    const std::vector<SpeechSegment> window_range{{0, window_size}};
    pending.reserve(window_size + stride_size);
    workspace.audio.reserve(window_size);
    const long long cpu_start = process_cpu_milliseconds();

    while (!shutdown_is_requested() &&
           (!capture.finished.load(std::memory_order_acquire) || capture.buffer.available() > 0)) {
        // The worker owns the drain, so the audio callback never allocates.
        const std::size_t before = all_samples.size();
        drain_capture(capture, all_samples, scratch, maximum_samples);
        if (all_samples.size() > before) {
            pending.insert(pending.end(), all_samples.begin() + before, all_samples.end());
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        while (pending.size() >= window_size) {
            const auto start = std::chrono::steady_clock::now();
            TranscriptionResult result;
            // Transcribe the leading window in place rather than copying it out.
            if (!run_transcription(context, pending, window_range, thread_count, workspace, result)) {
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

    const std::size_t dropped = capture.dropped_frames.load(std::memory_order_acquire);
    if (dropped > 0) {
        report_warning(ErrorCategory::Recording,
            "Streaming buffer overflowed; " + std::to_string(dropped) + " frames (" +
            std::to_string(capture.dropped_seconds(sample_rate)) + " s) were dropped.");
    }
    std::cout << "Streaming worker CPU time: "
              << process_cpu_milliseconds() - cpu_start << " ms" << std::endl;
}

// Prints the capture devices in a form both a person and the GUI can read.
bool list_capture_devices(AudioContext& context) {
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    const ma_result result = context.capture_devices(&infos, &count);
    if (result != MA_SUCCESS) {
        report_error(ErrorCategory::Microphone, describe_device_error(result));
        return false;
    }
    if (count == 0) {
        report_error(ErrorCategory::Microphone,
            "No capture device is available. Connect a microphone and try again.");
        return false;
    }

    for (ma_uint32 i = 0; i < count; ++i) {
        std::cout << "DEVICE|" << i << '|'
                  << (infos[i].isDefault ? "default" : "") << '|'
                  << infos[i].name << std::endl;
    }
    return true;
}

// Resolves the requested index to a device identifier. A negative index means
// the system default, which is left as a null identifier.
bool select_capture_device(
    AudioContext& context,
    int requested_index,
    ma_device_id& id,
    bool& has_id,
    std::string& name) {
    has_id = false;
    name = "system default";
    if (requested_index < 0) {
        return true;
    }

    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    const ma_result result = context.capture_devices(&infos, &count);
    if (result != MA_SUCCESS) {
        report_error(ErrorCategory::Microphone, describe_device_error(result));
        return false;
    }
    if (static_cast<ma_uint32>(requested_index) >= count) {
        report_error(ErrorCategory::Microphone,
            "Capture device " + std::to_string(requested_index) + " does not exist; " +
            std::to_string(count) + " device(s) are available. Use --list-devices.");
        return false;
    }

    id = infos[requested_index].id;
    has_id = true;
    name = infos[requested_index].name;
    return true;
}

struct PerformanceMetrics {
    long long model_load_ms = 0;
    long long vad_us = 0;
    long long transcription_ms = 0;
    long long total_ms = 0;
    long long cpu_ms = 0;
    long peak_memory_kb = 0;
    double audio_seconds = 0.0;

    // Below 1.0 the machine transcribes faster than the audio plays.
    double real_time_factor() const {
        return audio_seconds > 0.0 ? (transcription_ms / 1000.0) / audio_seconds : 0.0;
    }
};

void report_performance(const PerformanceMetrics& metrics) {
    std::cout << "PERF|model_load_ms=" << metrics.model_load_ms
              << "|vad_us=" << metrics.vad_us
              << "|transcription_ms=" << metrics.transcription_ms
              << "|total_ms=" << metrics.total_ms
              << "|cpu_ms=" << metrics.cpu_ms
              << "|peak_memory_kb=" << metrics.peak_memory_kb
              << "|rtf=" << metrics.real_time_factor() << std::endl;
}

// Runs the production pipeline (VAD then transcription) over fixed audio for
// every model and thread count, so numbers are comparable between runs.
bool run_benchmark(
    const std::vector<std::string>& model_paths,
    const std::vector<int>& thread_counts,
    const fs::path& input_path,
    std::uint32_t sample_rate) {
    std::vector<std::int16_t> samples;
    std::string reason;
    if (!read_wav(input_path, samples, sample_rate, reason)) {
        report_error(ErrorCategory::Recording, reason);
        return false;
    }

    const double audio_seconds = samples.size() / static_cast<double>(sample_rate);
    std::cout << "Benchmark input: " << input_path.string() << " (" << audio_seconds
              << " seconds)" << std::endl;

    const auto vad_start = std::chrono::steady_clock::now();
    const std::vector<SpeechSegment> segments = detect_speech_segments(samples, sample_rate);
    const long long vad_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - vad_start).count();
    if (segments.empty()) {
        report_error(ErrorCategory::Recording,
            "No speech detected in the benchmark input; results would be meaningless.");
        return false;
    }

    const std::vector<std::int16_t> speech = extract_speech(samples, segments);
    const double speech_seconds = speech.size() / static_cast<double>(sample_rate);
    const std::vector<SpeechSegment> speech_range{{0, speech.size()}};
    std::cout << "Speech extracted: " << speech_seconds << " seconds, VAD " << vad_us
              << " us\n" << std::endl;

    std::cout << std::left << std::setw(10) << "model" << std::setw(9) << "threads"
              << std::setw(10) << "load ms" << std::setw(14) << "transcribe ms"
              << std::setw(11) << "total ms" << std::setw(10) << "cpu ms"
              << std::setw(12) << "peak KB" << std::setw(8) << "RTF" << std::endl;

    for (const std::string& model_path : model_paths) {
        ModelMetadata metadata;
        if (!inspect_model(model_path, metadata, reason)) {
            report_error(ErrorCategory::Model, reason);
            return false;
        }

        for (const int threads : thread_counts) {
            const auto total_start = std::chrono::steady_clock::now();
            const long long cpu_start = process_cpu_milliseconds();

            const auto load_start = std::chrono::steady_clock::now();
            WhisperContext context(model_path);
            const long long load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - load_start).count();
            if (!context) {
                report_error(ErrorCategory::Model,
                    "The model header is valid but Whisper could not load it: " + model_path);
                return false;
            }

            TranscriptionWorkspace workspace;
            TranscriptionResult result;
            if (!run_transcription(context.get(), speech, speech_range, threads, workspace, result)) {
                return false;
            }

            PerformanceMetrics metrics;
            metrics.model_load_ms = load_ms;
            metrics.vad_us = vad_us;
            metrics.transcription_ms = result.milliseconds;
            metrics.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - total_start).count();
            metrics.cpu_ms = process_cpu_milliseconds() - cpu_start;
            metrics.peak_memory_kb = peak_memory_kb();
            metrics.audio_seconds = speech_seconds;

            std::cout << std::left << std::setw(10) << metadata.variant
                      << std::setw(9) << threads
                      << std::setw(10) << metrics.model_load_ms
                      << std::setw(14) << metrics.transcription_ms
                      << std::setw(11) << metrics.total_ms
                      << std::setw(10) << metrics.cpu_ms
                      << std::setw(12) << metrics.peak_memory_kb
                      << std::setw(8) << metrics.real_time_factor() << std::endl;
            report_performance(metrics);
        }
    }
    return true;
}

// Measured on a 12-thread machine over 12 s of speech (see docs/PERFORMANCE.md):
// going from 8 to 11 threads cut transcription time by about 7% while using
// roughly 24% more CPU. Eight is where the useful scaling stops, so leave the
// rest of the machine alone.
int default_thread_count() {
    constexpr unsigned int useful_maximum = 8;
    const unsigned int hardware_threads = std::thread::hardware_concurrency();
    if (hardware_threads <= 1) {
        return 1;
    }
    return static_cast<int>(std::min(useful_maximum, hardware_threads - 1));
}

bool parse_options(
    int argc,
    char** argv,
    int& thread_count,
    bool& compare_mode,
    bool& streaming_mode,
    int& duration_seconds,
    std::vector<std::string>& model_paths,
    std::vector<std::string>& model_directories,
    int& device_index,
    bool& list_devices,
    bool& benchmark_mode,
    std::string& input_path,
    bool& threads_explicit) {
    thread_count = default_thread_count();
    compare_mode = false;
    streaming_mode = false;
    duration_seconds = 15;
    model_paths.clear();
    model_directories.clear();
    device_index = -1;
    list_devices = false;
    benchmark_mode = false;
    input_path.clear();
    threads_explicit = false;

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
                threads_explicit = true;
            } catch (const std::exception&) {
                std::cerr << "Thread count must be a positive integer." << std::endl;
                return false;
            }
        } else if (argument == "--device") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --device." << std::endl;
                return false;
            }
            try {
                device_index = std::stoi(argv[++i]);
                if (device_index < 0) {
                    throw std::out_of_range("device index");
                }
            } catch (const std::exception&) {
                std::cerr << "Device index must be zero or a positive integer." << std::endl;
                return false;
            }
        } else if (argument == "--benchmark") {
            benchmark_mode = true;
        } else if (argument == "--input") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --input." << std::endl;
                return false;
            }
            input_path = argv[++i];
        } else if (argument == "--list-devices") {
            list_devices = true;
        } else if (argument == "--model-dir") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --model-dir." << std::endl;
                return false;
            }
            model_directories.emplace_back(argv[++i]);
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
                      << "  ./build-release/audio_to_text_cli MODEL_PATH [--threads N]\n"
                      << "      [--duration 15|45|60] [--stream] [--model-dir DIR]\n"
                      << "      [--device INDEX]\n"
                      << "  ./build-release/audio_to_text_cli --list-devices\n"
                      << "  ./build-release/audio_to_text_cli --benchmark --input SPEECH.wav\n"
                      << "      MODEL_PATH ... [--threads N]\n"
                      << "  ./build-release/audio_to_text_cli --compare MODEL_PATH MODEL_PATH ...\n"
                      << "      [--threads N] [--model-dir DIR]\n"
                      << "\nModels must sit inside ./models, the application data directory,\n"
                      << "or a directory approved with --model-dir.\n"
                      << "Recordings and transcripts are written to the application data directory."
                      << std::endl;
            return false;
        } else if (!argument.empty() && argument[0] == '-') {
            std::cerr << "Unknown option: " << argument << std::endl;
            return false;
        } else {
            model_paths.push_back(argument);
        }
    }

    if (list_devices) {
        return true;
    }
    if (benchmark_mode) {
        if (input_path.empty()) {
            std::cerr << "--benchmark requires --input PATH (mono 16-bit WAV at 16 kHz)." << std::endl;
            return false;
        }
        if (model_paths.empty()) {
            model_paths.emplace_back("models/ggml-base.en.bin");
        }
        return true;
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
            report_error(ErrorCategory::Model,
                "The model header is valid but Whisper could not load it: " + model_path);
            return false;
        }

        TranscriptionResult original_result;
        TranscriptionResult cleaned_result;
        TranscriptionWorkspace workspace;
        const bool success = run_transcription(
                context.get(), original, speech_range, thread_count, workspace, original_result) &&
            run_transcription(context.get(), cleaned, speech_range, thread_count, workspace, cleaned_result);
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

    int thread_count = 0;
    bool compare_mode = false;
    bool streaming_mode = false;
    int duration_seconds = 15;
    std::vector<std::string> model_paths;
    std::vector<std::string> model_directories;
    int device_index = -1;
    bool list_devices = false;
    bool benchmark_mode = false;
    std::string input_path;
    bool threads_explicit = false;
    if (!parse_options(argc, argv, thread_count, compare_mode, streaming_mode, duration_seconds,
                       model_paths, model_directories, device_index, list_devices,
                       benchmark_mode, input_path, threads_explicit)) {
        return argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") ? 0 : 1;
    }

    install_shutdown_handlers();

    AudioContext audio_context;
    const ma_result context_result = audio_context.initialize();
    if (context_result != MA_SUCCESS) {
        report_error(ErrorCategory::Microphone, describe_device_error(context_result));
        return 1;
    }

    if (list_devices) {
        return list_capture_devices(audio_context) ? 0 : 1;
    }

    const fs::path data_directory = application_data_directory();
    const fs::path recordings_directory = data_directory / "recordings";
    const fs::path transcripts_directory = data_directory / "transcripts";
    if (!ensure_private_directory(data_directory) ||
        !ensure_private_directory(recordings_directory) ||
        !ensure_private_directory(transcripts_directory)) {
        report_error(ErrorCategory::FileSaving,
            "Could not create the application data directory: " + data_directory.string());
        return 1;
    }
    std::cout << "Data directory: " << data_directory.string() << std::endl;

    std::vector<fs::path> model_roots{
        fs::current_path() / "models",
        data_directory / "models"
    };
    for (const std::string& directory : model_directories) {
        model_roots.emplace_back(directory);
    }

    // Every model is resolved and inspected before the microphone is touched.
    std::vector<std::string> approved_models;
    std::vector<ModelMetadata> model_metadata;
    approved_models.reserve(model_paths.size());
    model_metadata.reserve(model_paths.size());
    for (const std::string& requested : model_paths) {
        fs::path approved;
        std::string reason;
        if (!resolve_model_path(requested, model_roots, approved, reason)) {
            report_error(ErrorCategory::Model, reason);
            return 1;
        }

        ModelMetadata metadata;
        if (!inspect_model(approved, metadata, reason)) {
            report_error(ErrorCategory::Model, reason);
            return 1;
        }

        report_model_observations(metadata);
        report_model(metadata);
        approved_models.push_back(approved.string());
        model_metadata.push_back(metadata);
    }
    model_paths = approved_models;

    if (benchmark_mode) {
        // The plan's sweep, minus anything this machine cannot actually run.
        const unsigned int available = std::thread::hardware_concurrency();
        std::vector<int> thread_counts;
        if (threads_explicit) {
            thread_counts.push_back(thread_count);
        } else {
            for (const int candidate : {2, 4, 8, 11}) {
                if (available == 0 || candidate <= static_cast<int>(available)) {
                    thread_counts.push_back(candidate);
                } else {
                    std::cout << "Skipping " << candidate << " threads; this machine reports "
                              << available << "." << std::endl;
                }
            }
        }
        return run_benchmark(model_paths, thread_counts, input_path, sample_rate) ? 0 : 1;
    }

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
            report_error(ErrorCategory::Model,
                "The model header is valid but Whisper could not load it. The file is "
                "probably corrupted or was built for a different whisper.cpp version: " +
                model_paths[0]);
            return 1;
        }
    }

    ma_device_id selected_id{};
    bool has_selected_id = false;
    std::string selected_name;
    if (!select_capture_device(audio_context, device_index, selected_id, has_selected_id, selected_name)) {
        return 1;
    }

    const std::size_t maximum_samples = static_cast<std::size_t>(sample_rate) * duration_seconds;
    AudioCapture capture(maximum_samples);
    std::vector<std::int16_t> recorded_samples;
    std::vector<std::int16_t> drain_scratch;
    TranscriptionWorkspace workspace;
    recorded_samples.reserve(maximum_samples);

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_s16;
    config.capture.channels = 1;
    config.capture.pDeviceID = has_selected_id ? &selected_id : nullptr;
    config.sampleRate = sample_rate;
    config.dataCallback = capture_callback;
    config.notificationCallback = capture_notification_callback;
    config.pUserData = &capture;

    CaptureDevice device;
    const ma_result device_result = device.initialize(audio_context.get(), config);
    if (device_result != MA_SUCCESS) {
        report_error(ErrorCategory::Microphone, describe_device_error(device_result));
        return 1;
    }

    const std::string active_name = device.name().empty() ? selected_name : device.name();
    std::cout << "INPUT|" << active_name << std::endl;
    std::cout << "Microphone: " << active_name << std::endl;

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

        capture.reset();
        recorded_samples.clear();

        const ma_result start_result = device.start();
        if (start_result != MA_SUCCESS) {
            report_error(ErrorCategory::Microphone, describe_device_error(start_result));
            break;
        }

        if (streaming_mode) {
            ScopedThread worker(std::thread(
                streaming_worker, std::ref(capture), std::ref(recorded_samples), maximum_samples,
                context.get(), thread_count, sample_rate));
            std::cout << "Streaming recording... Press Enter to stop." << std::endl;
            read_command(command);
            capture.expected_stop.store(true, std::memory_order_release);
            device.stop();
            capture.finished.store(true, std::memory_order_release);
            worker.join();
        } else {
            std::cout << "Recording... Press Enter to stop." << std::endl;
            read_command(command);
            capture.expected_stop.store(true, std::memory_order_release);
            device.stop();
            capture.finished.store(true, std::memory_order_release);
        }

        // Whatever mode ran, anything still buffered belongs to this recording.
        drain_capture(capture, recorded_samples, drain_scratch, maximum_samples);

        if (shutdown_is_requested()) {
            break;
        }

        if (capture.device_lost.load(std::memory_order_acquire)) {
            report_error(ErrorCategory::Microphone,
                "The microphone stopped or changed during recording. Reconnect it and record again.");
            continue;
        }

        if (recorded_samples.empty()) {
            report_error(ErrorCategory::Recording, "No audio was captured. Check the microphone and try again.");
            continue;
        }

        const std::size_t dropped = capture.dropped_frames.load(std::memory_order_acquire);
        if (dropped > 0) {
            report_warning(ErrorCategory::Recording,
                "Reached the " + std::to_string(duration_seconds) + " second limit or could not keep up; " +
                std::to_string(dropped) + " frames (" +
                std::to_string(capture.dropped_seconds(sample_rate)) + " s) were dropped.");
        }

        const std::string stamp = session_stamp();
        const fs::path recording_path = recordings_directory / (stamp + "-recording.wav");
        const fs::path cleaned_path = recordings_directory / (stamp + "-cleaned.wav");
        const fs::path speech_path = recordings_directory / (stamp + "-speech.wav");
        const fs::path transcript_path = transcripts_directory / (stamp + "-transcription.txt");
        if (!output_path_is_safe(recording_path, data_directory) ||
            !output_path_is_safe(cleaned_path, data_directory) ||
            !output_path_is_safe(speech_path, data_directory) ||
            !output_path_is_safe(transcript_path, data_directory)) {
            break;
        }

        if (!write_wav(recording_path, recorded_samples, sample_rate)) {
            report_error(ErrorCategory::FileSaving, "Could not save " + recording_path.string() + ".");
            break;
        }

        report_saved("RECORDING", recording_path);
        std::cout << "Recorded "
                  << recorded_samples.size() / static_cast<double>(sample_rate)
                  << " seconds." << std::endl;

        const NoiseProfile noise_profile = measure_noise_floor(recorded_samples, sample_rate);
        const std::vector<std::int16_t> cleaned_samples = reduce_noise(recorded_samples, noise_profile);
        if (!write_wav(cleaned_path, cleaned_samples, sample_rate)) {
            report_error(ErrorCategory::FileSaving, "Could not save " + cleaned_path.string() + ".");
            break;
        }

        const float noise_db = noise_profile.noise_rms > 0.0f
            ? 20.0f * std::log10(noise_profile.noise_rms)
            : -std::numeric_limits<float>::infinity();
        std::cout << "Measured noise floor: " << noise_profile.noise_rms
                  << " RMS (" << noise_db << " dBFS), attenuation threshold: "
                  << noise_profile.attenuation_threshold << std::endl;
        report_saved("CLEANED", cleaned_path);

        const auto vad_start = std::chrono::steady_clock::now();
        const std::vector<SpeechSegment> speech_segments = detect_speech_segments(recorded_samples, sample_rate);
        const auto vad_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - vad_start).count();

        if (speech_segments.empty()) {
            report_error(ErrorCategory::Recording, "No speech detected. Try speaking closer to the microphone.");
            continue;
        }

        const std::vector<std::int16_t> speech = extract_speech(recorded_samples, speech_segments);
        const std::vector<std::int16_t> cleaned_speech = extract_speech(cleaned_samples, speech_segments);
        if (!write_wav(speech_path, speech, sample_rate)) {
            report_error(ErrorCategory::FileSaving, "Could not save " + speech_path.string() + ".");
            break;
        }

        const double raw_seconds = recorded_samples.size() / static_cast<double>(sample_rate);
        const double speech_seconds = speech.size() / static_cast<double>(sample_rate);
        const double reduction = raw_seconds > 0.0 ? (1.0 - speech_seconds / raw_seconds) * 100.0 : 0.0;
        std::cout << "Speech detected: " << speech_seconds << " seconds in "
                  << speech_segments.size() << " segment(s)." << std::endl;
        report_saved("SPEECH", speech_path);
        std::cout << "Removed " << reduction << "% of recorded audio."
                  << " VAD time: " << vad_ms << " ms" << std::endl;

        if (compare_mode) {
            if (!compare_models(model_paths, speech, cleaned_speech, thread_count)) {
                break;
            }
        } else if (context) {
            const std::vector<SpeechSegment> speech_range{{0, speech.size()}};
            if (!transcribe(context.get(), speech, speech_range, thread_count, workspace, transcript_path)) {
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
