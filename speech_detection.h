#pragma once

// Noise-floor measurement, attenuation, and voice activity detection.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "whisper.h"

struct SpeechSegment {
    std::size_t begin;
    std::size_t end;
};

struct NoiseProfile {
    float noise_rms;
    float attenuation_threshold;
};

inline NoiseProfile measure_noise_floor(
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

inline std::vector<std::int16_t> reduce_noise(
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

inline std::vector<SpeechSegment> detect_speech_segments(
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

inline std::vector<std::int16_t> extract_speech(
    const std::vector<std::int16_t>& samples,
    const std::vector<SpeechSegment>& segments) {
    std::vector<std::int16_t> speech;
    for (const SpeechSegment& segment : segments) {
        speech.insert(speech.end(), samples.begin() + segment.begin, samples.begin() + segment.end);
    }
    return speech;
}
