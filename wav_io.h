#pragma once

// Reading and writing the recorder's WAV format.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>
#include "file_storage.h"

inline void append_little_endian(std::vector<char>& buffer, std::uint32_t value) {
    buffer.push_back(static_cast<char>(value & 0xff));
    buffer.push_back(static_cast<char>((value >> 8) & 0xff));
    buffer.push_back(static_cast<char>((value >> 16) & 0xff));
    buffer.push_back(static_cast<char>((value >> 24) & 0xff));
}

inline bool write_wav(const fs::path& path, const std::vector<std::int16_t>& samples, std::uint32_t sample_rate) {
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
inline bool read_wav(
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
