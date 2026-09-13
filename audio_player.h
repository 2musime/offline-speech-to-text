#pragma once

// Playing back a recording. Uses the vendored miniaudio rather than Qt
// Multimedia, so the application keeps its single runtime dependency and the
// package does not grow a media framework.
//
// The whole recording is decoded into memory before playback starts. A ten
// minute capture is about 19 MB, and holding it means the audio callback only
// ever copies from a buffer that is already there: no file reads, no locks and
// no allocation on the audio thread.

#include "miniaudio.h"
#include "wav_io.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// The recorder only ever writes mono 16-bit at this rate, and read_wav refuses
// anything else, so playback does not have to negotiate a format.
inline constexpr std::uint32_t playback_sample_rate = 16000;

class AudioPlayer {
public:
    AudioPlayer() = default;

    ~AudioPlayer() {
        unload();
    }

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;
    AudioPlayer(AudioPlayer&&) = delete;
    AudioPlayer& operator=(AudioPlayer&&) = delete;

    bool load(const fs::path& path, std::string& reason) {
        unload();
        if (!read_wav(path, samples_, playback_sample_rate, reason)) {
            return false;
        }

        position_.store(0, std::memory_order_release);
        finished_.store(false, std::memory_order_release);

        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_s16;
        config.playback.channels = 1;
        config.sampleRate = playback_sample_rate;
        config.dataCallback = playback_callback;
        config.pUserData = this;
        if (ma_device_init(nullptr, &config, &device_) != MA_SUCCESS) {
            samples_.clear();
            reason = "No playback device is available.";
            return false;
        }
        initialised_ = true;
        return true;
    }

    void unload() {
        stop();
        if (initialised_) {
            ma_device_uninit(&device_);
            initialised_ = false;
        }
        samples_.clear();
        position_.store(0, std::memory_order_release);
    }

    bool play() {
        if (!initialised_ || samples_.empty()) {
            return false;
        }
        // Reaching the end and pressing play again starts over rather than
        // doing nothing, which is what every other player does.
        if (finished_.load(std::memory_order_acquire)) {
            position_.store(0, std::memory_order_release);
            finished_.store(false, std::memory_order_release);
        }
        if (ma_device_start(&device_) != MA_SUCCESS) {
            return false;
        }
        playing_ = true;
        return true;
    }

    void pause() {
        if (initialised_ && playing_) {
            ma_device_stop(&device_);
            playing_ = false;
        }
    }

    void stop() {
        pause();
        position_.store(0, std::memory_order_release);
        finished_.store(false, std::memory_order_release);
    }

    void seek_seconds(double seconds) {
        const std::size_t frame =
            static_cast<std::size_t>(seconds * playback_sample_rate);
        position_.store(std::min(frame, samples_.size()), std::memory_order_release);
        finished_.store(false, std::memory_order_release);
    }

    bool is_playing() const { return playing_; }
    bool is_loaded() const { return initialised_ && !samples_.empty(); }
    bool has_finished() const { return finished_.load(std::memory_order_acquire); }

    double duration_seconds() const {
        return static_cast<double>(samples_.size()) / playback_sample_rate;
    }

    double position_seconds() const {
        return static_cast<double>(position_.load(std::memory_order_acquire)) /
            playback_sample_rate;
    }

private:
    // Copies from the loaded buffer and advances. Nothing here allocates, locks
    // or touches the filesystem.
    static void playback_callback(ma_device* device, void* output, const void*,
                                  ma_uint32 frame_count) {
        auto* player = static_cast<AudioPlayer*>(device->pUserData);
        auto* out = static_cast<std::int16_t*>(output);

        const std::size_t total = player->samples_.size();
        const std::size_t start = player->position_.load(std::memory_order_acquire);
        const std::size_t available = start < total ? total - start : 0;
        const std::size_t count = std::min<std::size_t>(frame_count, available);

        for (std::size_t i = 0; i < count; ++i) {
            out[i] = player->samples_[start + i];
        }
        // Silence past the end, rather than whatever the buffer held before.
        for (std::size_t i = count; i < frame_count; ++i) {
            out[i] = 0;
        }

        player->position_.store(start + count, std::memory_order_release);
        if (count < frame_count) {
            player->finished_.store(true, std::memory_order_release);
        }
    }

    ma_device device_{};
    std::vector<std::int16_t> samples_;
    std::atomic<std::size_t> position_{0};
    std::atomic<bool> finished_{false};
    bool initialised_ = false;
    bool playing_ = false;
};
