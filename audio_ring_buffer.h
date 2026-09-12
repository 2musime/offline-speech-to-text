#pragma once

// Single-producer, single-consumer ring buffer for real-time audio capture.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

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

// Moves buffered audio into `samples`, never past `limit`. Runs on a normal
// thread; the reserved capacity means the insert does not allocate.
inline std::size_t drain_capture(
    AudioCapture& capture,
    std::vector<std::int16_t>& samples,
    std::vector<std::int16_t>& scratch,
    std::size_t limit) {
    constexpr std::size_t chunk = 4096;
    std::size_t moved = 0;
    // The buffer is emptied whatever happens, including after the duration limit
    // is reached. Stopping early would leave audio in it, and the streaming
    // worker's loop waits on exactly that condition: it would spin forever, the
    // recording would never be transcribed, and the interface would sit on a
    // progress bar that moves without anything happening behind it.
    while (true) {
        const std::size_t count = capture.buffer.read(scratch, chunk);
        if (count == 0) {
            break;
        }
        const std::size_t keep = samples.size() < limit
            ? std::min(count, limit - samples.size())
            : 0;
        if (keep > 0) {
            samples.insert(samples.end(), scratch.begin(), scratch.begin() + keep);
            moved += keep;
        }
        if (keep < count) {
            // Past the limit: consumed so the buffer drains, but not kept.
            capture.dropped_frames.fetch_add(count - keep, std::memory_order_relaxed);
        }
    }
    return moved;
}
