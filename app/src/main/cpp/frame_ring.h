#pragma once
// Ring buffer lock-free (satu penulis / satu pembaca) untuk frame stereo interleaved
// (L, R, L, R, ...). Dipakai untuk audio internal HP: thread tangkap (AudioRecord)
// menulis, thread audio Oboe membaca.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

class FrameRing {
public:
    FrameRing() : buf(kFrames * 2, 0.f) {}

    // Panggil hanya saat pembaca tidak sedang aktif.
    void clear() { tail.store(head.load(std::memory_order_acquire), std::memory_order_release); }

    size_t available() const {
        return head.load(std::memory_order_acquire) - tail.load(std::memory_order_acquire);
    }

    // Dari thread tangkap. Mengembalikan jumlah frame yang masuk (sisanya dibuang kalau penuh).
    size_t write(const float* in, size_t frames) {
        const size_t h = head.load(std::memory_order_relaxed);
        const size_t t = tail.load(std::memory_order_acquire);
        const size_t n = std::min(frames, kFrames - (h - t));
        for (size_t i = 0; i < n; ++i) {
            buf[((h + i) & kMask) * 2] = in[i * 2];
            buf[((h + i) & kMask) * 2 + 1] = in[i * 2 + 1];
        }
        head.store(h + n, std::memory_order_release);
        return n;
    }

    // Dari thread audio.
    size_t read(float* out, size_t frames) {
        const size_t t = tail.load(std::memory_order_relaxed);
        const size_t h = head.load(std::memory_order_acquire);
        const size_t n = std::min(frames, h - t);
        for (size_t i = 0; i < n; ++i) {
            out[i * 2] = buf[((t + i) & kMask) * 2];
            out[i * 2 + 1] = buf[((t + i) & kMask) * 2 + 1];
        }
        tail.store(t + n, std::memory_order_release);
        return n;
    }

    // Dari thread audio: buang frame tertua (mengejar kalau tertinggal).
    void skip(size_t frames) {
        const size_t t = tail.load(std::memory_order_relaxed);
        const size_t h = head.load(std::memory_order_acquire);
        tail.store(t + std::min(frames, h - t), std::memory_order_release);
    }

private:
    static constexpr size_t kFrames = static_cast<size_t>(1) << 16;  // ~1,4 detik @48 kHz
    static constexpr size_t kMask = kFrames - 1;
    std::vector<float> buf;
    std::atomic<size_t> head{0};
    std::atomic<size_t> tail{0};
};
