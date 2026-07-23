// Lock-free single-producer single-consumer ring buffer for audio.
//
// Used by all audio backends to transfer stereo s16 PCM data from the
// guest audio thread (producer) to the host audio callback (consumer)
// without per-sample locking.

#pragma once
#include "../../common/types.h"
#include <atomic>
#include <cstring>
#include <memory>

namespace HLE {
namespace Audio {

// Ring buffer of stereo s16 samples (interleaved L,R,L,R,...).
// SPSC: safe for one writer + one reader without external locking.
class StereoRingBuffer {
public:
    explicit StereoRingBuffer(size_t capacity_frames)
        : m_capacity(capacity_frames * 2)  // stereo
        , m_buffer(std::make_unique<s16[]>(m_capacity))
    {}

    // Push `frames` of stereo s16 data.  Returns frames actually written
    // (0 if full, or partial).  Never blocks.
    size_t Push(const s16* data, size_t frames) {
        const size_t samples = frames * 2;
        const size_t w = m_write.load(std::memory_order_relaxed);
        const size_t r = m_read.load(std::memory_order_acquire);

        size_t avail = m_capacity - (w - r);
        if (avail < samples) {
            if (avail < 2) return 0;  // completely full
            // Partial write: fill what we can.
            const size_t partial = avail & ~1ULL;  // even sample count
            WriteAt(w, data, partial);
            m_write.store(w + partial, std::memory_order_release);
            return partial / 2;
        }

        WriteAt(w, data, samples);
        m_write.store(w + samples, std::memory_order_release);
        return frames;
    }

    // Pop up to `max_frames` frames.  Returns frames actually read.
    size_t Pop(s16* out, size_t max_frames) {
        const size_t max_samples = max_frames * 2;
        const size_t w = m_write.load(std::memory_order_acquire);
        const size_t r = m_read.load(std::memory_order_relaxed);
        const size_t avail = w - r;
        if (avail == 0) return 0;

        const size_t samples = (avail < max_samples) ? avail : max_samples;
        ReadAt(r, out, samples);
        m_read.store(r + samples, std::memory_order_release);
        return samples / 2;
    }

    // Available frames for reading.
    size_t Available() const {
        return (m_write.load(std::memory_order_acquire) -
                m_read.load(std::memory_order_relaxed)) / 2;
    }

    // Remaining write capacity in frames.
    size_t Remaining() const {
        return (m_capacity - (m_write.load(std::memory_order_relaxed) -
                              m_read.load(std::memory_order_acquire))) / 2;
    }

    // Reset to empty state.
    void Reset() {
        m_read.store(m_write.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    // Clear all data.
    void Clear() {
        m_write.store(0, std::memory_order_relaxed);
        m_read.store(0, std::memory_order_relaxed);
    }

    size_t Capacity() const { return m_capacity / 2; }
    bool   Empty() const { return Available() == 0; }
    bool   Full()  const { return Remaining() == 0; }

private:
    void WriteAt(size_t offset, const s16* data, size_t samples) {
        const size_t idx = offset % m_capacity;
        const size_t first = (std::min)(samples, m_capacity - idx);
        std::memcpy(m_buffer.get() + idx, data, first * sizeof(s16));
        if (first < samples) {
            std::memcpy(m_buffer.get(), data + first,
                       (samples - first) * sizeof(s16));
        }
    }

    void ReadAt(size_t offset, s16* out, size_t samples) const {
        const size_t idx = offset % m_capacity;
        const size_t first = (std::min)(samples, m_capacity - idx);
        std::memcpy(out, m_buffer.get() + idx, first * sizeof(s16));
        if (first < samples) {
            std::memcpy(out + first, m_buffer.get(),
                       (samples - first) * sizeof(s16));
        }
    }

    const size_t m_capacity;
    std::unique_ptr<s16[]> m_buffer;
    std::atomic<size_t> m_write{0};
    std::atomic<size_t> m_read{0};
};

} // namespace Audio
} // namespace HLE
