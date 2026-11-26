#pragma once
// Ring buffer lock-free satu-penulis (thread audio) / satu-pembaca (thread rekam).
// Menyimpan pasangan float (asli, hasil) secara berurutan.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

class RingBuffer {
public:
    RingBuffer() : buf(kSize, 0.f) {}

    // Panggil hanya saat penulis tidak sedang aktif.
    void clear() { tail.store(head.load(std::memory_order_acquire), std::memory_order_release); }

    // Dari thread audio. Mengembalikan false kalau penuh (pasangan dibuang).
    bool push2(float a, float b) {
        const size_t h = head.load(std::memory_order_relaxed);
        const size_t t = tail.load(std::memory_order_acquire);
        if (h - t > kSize - 2) return false;
        buf[h & kMask] = a;
        buf[(h + 1) & kMask] = b;
        head.store(h + 2, std::memory_order_release);
        return true;
    }

    // Dari thread pembaca. Mengembalikan jumlah float (selalu genap).
    size_t read(float* out, size_t maxFloats) {
        const size_t t = tail.load(std::memory_order_relaxed);
        const size_t h = head.load(std::memory_order_acquire);
        size_t n = std::min(h - t, maxFloats);
        n &= ~static_cast<size_t>(1);
        for (size_t i = 0; i < n; ++i) out[i] = buf[(t + i) & kMask];
        tail.store(t + n, std::memory_order_release);
        return n;
    }

private:
    static constexpr size_t kSize = static_cast<size_t>(1) << 20;  // ~11 detik @48 kHz
    static constexpr size_t kMask = kSize - 1;
    std::vector<float> buf;
    std::atomic<size_t> head{0};
    std::atomic<size_t> tail{0};
};
