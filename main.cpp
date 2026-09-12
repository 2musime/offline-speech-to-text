#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "whisper.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

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

bool transcribe(const char* model_path, const std::vector<std::int16_t>& samples) {
    whisper_context_params context_params = whisper_context_default_params();
    whisper_context* context = whisper_init_from_file_with_params(model_path, context_params);
    if (context == nullptr) {
        std::cerr << "Could not load Whisper model: " << model_path << std::endl;
        return false;
    }

    std::vector<float> audio(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        audio[i] = static_cast<float>(samples[i]) / 32768.0f;
    }

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.print_progress = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    params.single_segment = false;
    params.language = "en";

    if (whisper_full(context, params, audio.data(), audio.size()) != 0) {
        std::cerr << "Whisper could not process the recording." << std::endl;
        whisper_free(context);
        return false;
    }

    std::string transcription;
    for (int i = 0; i < whisper_full_n_segments(context); ++i) {
        transcription += whisper_full_get_segment_text(context, i);
    }
    whisper_free(context);

    std::ofstream file("transcription.txt");
    if (!file) {
        std::cerr << "Could not save transcription.txt." << std::endl;
        return false;
    }
    file << transcription << '\n';

    std::cout << "\nTranscription:\n" << transcription << std::endl;
    std::cout << "Saved transcription.txt" << std::endl;
    return true;
}

int main(int argc, char** argv) {
    constexpr ma_uint32 sample_rate = WHISPER_SAMPLE_RATE;
    Recording recording;

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

    std::cout << "Press Enter to start recording." << std::endl;
    std::cin.get();

    if (ma_device_start(&device) != MA_SUCCESS) {
        std::cerr << "Could not start recording." << std::endl;
        ma_device_uninit(&device);
        return 1;
    }

    std::cout << "Recording... Press Enter to stop." << std::endl;
    std::cin.get();
    ma_device_stop(&device);
    ma_device_uninit(&device);

    if (!write_wav("recording.wav", recording.samples, sample_rate)) {
        std::cerr << "Could not save recording.wav." << std::endl;
        return 1;
    }

    std::cout << "Saved recording.wav ("
              << recording.samples.size() / static_cast<double>(sample_rate)
              << " seconds)." << std::endl;

    if (argc < 2) {
        std::cout << "To transcribe it, run again with a model path, for example:\n"
                  << "  ./build/audio_to_text models/ggml-base.en.bin" << std::endl;
        return 0;
    }

    if (!transcribe(argv[1], recording.samples)) {
        return 1;
    }
    return 0;
}