#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "whisper.h"

#include <algorithm>
#include <cstdint>
#include <chrono>
#include <cmath>
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
#include <sys/resource.h>
#endif

struct Recording {
    std::mutex mutex;
    std::vector<std::int16_t> samples;
};

void capture_callback(ma_device* device, void*, const void* input, ma_uint32 frame_count) {
    if (input == nullptr) {
        return;
    }

    auto* recording = static_cast<Recording*>(device->pUserData);
    const auto* samples = static_cast<const std::int16_t*>(input);

    std::lock_guard<std::mutex> lock(recording->mutex);
    recording->samples.insert(recording->samples.end(), samples, samples + frame_count);
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
    file.put(1);
    file.put(0);
    file.put(1);
    file.put(0);
    write_little_endian(file, sample_rate);
    write_little_endian(file, sample_rate * sizeof(std::int16_t));
    file.put(sizeof(std::int16_t));
    file.put(0);
    file.put(16);
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
            std::cerr << "Whisper could not process the speech segment." << std::endl;
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
        std::cerr << "Could not save transcription.txt." << std::endl;
        return false;
    }
    file << result.text << '\n';

    std::cout << "\nTranscription:\n" << result.text << std::endl;
    std::cout << "Saved transcription.txt" << std::endl;
    std::cout << "Whisper processing time: " << result.milliseconds << " ms" << std::endl;
    return true;
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
    std::vector<std::string>& model_paths) {
    thread_count = default_thread_count();
    compare_mode = false;
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
        } else if (argument == "--help" || argument == "-h") {
            std::cout << "Usage:\n"
                      << "  ./build/audio_to_text MODEL_PATH [--threads N]\n"
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
    const std::vector<std::int16_t>& samples,
    const std::vector<SpeechSegment>& segments,
    int thread_count) {
    std::cout << "\nModel comparison (same audio and VAD segments):\n";
    for (const std::string& model_path : model_paths) {
        whisper_context_params context_params = whisper_context_default_params();
        whisper_context* context = whisper_init_from_file_with_params(model_path.c_str(), context_params);
        if (context == nullptr) {
            std::cerr << "Could not load Whisper model: " << model_path << std::endl;
            return false;
        }

        TranscriptionResult result;
        const bool success = run_transcription(context, samples, segments, thread_count, result);
        const long memory_kb = peak_memory_kb();
        whisper_free(context);
        if (!success) {
            return false;
        }

        std::error_code error;
        const auto model_size = std::filesystem::file_size(model_path, error);
        const double model_size_mb = error ? 0.0 : model_size / (1024.0 * 1024.0);
        std::cout << "\nModel: " << model_path
                  << "\n  Size: " << model_size_mb << " MB"
                  << "\n  Processing time: " << result.milliseconds << " ms"
                  << "\n  Peak memory: " << memory_kb << " KB"
                  << "\n  Transcription: " << result.text << std::endl;
    }
    return true;
}

int main(int argc, char** argv) {
    constexpr ma_uint32 sample_rate = WHISPER_SAMPLE_RATE;
    Recording recording;

    int thread_count = 0;
    bool compare_mode = false;
    std::vector<std::string> model_paths;
    if (!parse_options(argc, argv, thread_count, compare_mode, model_paths)) {
        return argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") ? 0 : 1;
    }

    const unsigned int hardware_threads = std::thread::hardware_concurrency();
    std::cout << "CPU threads detected: " << (hardware_threads == 0 ? 1 : hardware_threads)
              << ", Whisper threads: " << thread_count << std::endl;

    whisper_context* context = nullptr;
    if (!compare_mode) {
        whisper_context_params context_params = whisper_context_default_params();
        context = whisper_init_from_file_with_params(model_paths[0].c_str(), context_params);
        if (context == nullptr) {
            std::cerr << "Could not load Whisper model: " << model_paths[0] << std::endl;
            return 1;
        }
    }

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_s16;
    config.capture.channels = 1;
    config.sampleRate = sample_rate;
    config.dataCallback = capture_callback;
    config.pUserData = &recording;

    ma_device device;
    if (ma_device_init(nullptr, &config, &device) != MA_SUCCESS) {
        std::cerr << "Could not open the microphone." << std::endl;
        return 1;
    }

    while (true) {
        std::cout << "Press Enter to start recording, or type q to quit." << std::endl;
        std::string command;
        std::getline(std::cin, command);
        if (!std::cin || command == "q" || command == "Q") {
            break;
        }

        {
            std::lock_guard<std::mutex> lock(recording.mutex);
            recording.samples.clear();
        }

        if (ma_device_start(&device) != MA_SUCCESS) {
            std::cerr << "Could not start recording." << std::endl;
            break;
        }

        std::cout << "Recording... Press Enter to stop." << std::endl;
        std::getline(std::cin, command);
        ma_device_stop(&device);

        if (!write_wav("recording.wav", recording.samples, sample_rate)) {
            std::cerr << "Could not save recording.wav." << std::endl;
            break;
        }

        std::cout << "Saved recording.wav ("
                  << recording.samples.size() / static_cast<double>(sample_rate)
                  << " seconds)." << std::endl;

        const auto vad_start = std::chrono::steady_clock::now();
        const std::vector<SpeechSegment> speech_segments = detect_speech_segments(recording.samples, sample_rate);
        const auto vad_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - vad_start).count();

        if (speech_segments.empty()) {
            std::cerr << "No speech detected. Try speaking closer to the microphone." << std::endl;
            continue;
        }

        const std::vector<std::int16_t> speech = extract_speech(recording.samples, speech_segments);
        if (!write_wav("speech.wav", speech, sample_rate)) {
            std::cerr << "Could not save speech.wav." << std::endl;
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
            if (!compare_models(model_paths, recording.samples, speech_segments, thread_count)) {
                break;
            }
        } else if (context != nullptr && !transcribe(context, recording.samples, speech_segments, thread_count)) {
            break;
        }
    }

    ma_device_uninit(&device);
    whisper_free(context);
    return 0;
}