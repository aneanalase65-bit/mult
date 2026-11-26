#include <cstdio>
#include <vector>
#include "../../app/src/main/cpp/compressor.h"
int main() {
    const float SR = 48000.f;
    StereoChain ch; ch.reset();
    ProcParams p; p.ceilDb = -1; p.limOn = true; p.mb.on = false;
    float maxJump = 0, prev = 0, pk = 0; bool bad = false; float maxRed = 0;
    for (int i = 0; i < (int)(SR * 12); ++i) {
        if (i % 256 == 0) {
            p.mb.on = (i / (int)SR) % 4 >= 2;   // ganti mode tiap 2 detik: dBFS <-> multiband
            ch.prepare(SR, p);
        }
        float t = i / SR;
        float env = 0.5f + 0.5f * std::sin(6.2831853f * 2.f * t);
        float x = 0.2f * env * (std::sin(6.2831853f * 220.f * t) + 0.5f * std::sin(6.2831853f * 660.f * t));
        float yl, yr, red;
        ch.process(x, x, false, p, yl, yr, red);
        if (!std::isfinite(yl) || !std::isfinite(yr)) bad = true;
        if (std::fabs(yl - prev) > 0.03f) printf("  lompatan %.3f di t=%.4f dtk\n", std::fabs(yl - prev), t); maxJump = std::max(maxJump, std::fabs(yl - prev)); prev = yl;
        pk = std::max(pk, std::fabs(yl)); maxRed = std::max(maxRed, red);
    }
    printf("NaN: %s | puncak %.3f (ceiling 0.891) | lompatan sampel terbesar %.3f | GR maks %.1f dB\n",
           bad ? "ADA" : "tidak", pk, maxJump, maxRed);
    // stereo path (reverb aktif -> stereo) tidak boleh crash
    p.mb.on = true; ch.prepare(SR, p);
    float yl, yr, red; ch.process(0.1f, 0.1f, true, p, yl, yr, red);
    printf("jalur stereo+MB: yl=%.4f yr=%.4f\n", yl, yr);
}
