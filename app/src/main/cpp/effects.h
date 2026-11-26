#pragma once
// Efek tambahan: high-pass (biquad), noise gate, dan reverb gaya Freeverb
// (8 comb + 4 allpass per sisi, kiri-kanan dibuat berbeda supaya melebar).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

inline float flushDenormal(float v) { return std::fabs(v) < 1e-20f ? 0.f : v; }

// High-pass orde-2 (Butterworth), membuang dengung rendah dan suara genggaman mic.
class HighPass {
public:
    void setup(float sr, float freq) {
        if (sr == lastSr && freq == lastFreq) return;
        lastSr = sr;
        lastFreq = freq;
        const float f = std::min(std::max(freq, 10.f), sr * 0.45f);
        const float w0 = 6.2831853f * f / sr;
        const float cw = std::cos(w0);
        const float alpha = std::sin(w0) / (2.f * 0.70710678f);
        const float a0 = 1.f + alpha;
        b0 = (1.f + cw) * 0.5f / a0;
        b1 = -(1.f + cw) / a0;
        b2 = b0;
        a1 = -2.f * cw / a0;
        a2 = (1.f - alpha) / a0;
    }
    void reset() { z1 = z2 = 0.f; }
    float process(float x) {
        const float y = b0 * x + z1;
        z1 = flushDenormal(b1 * x - a1 * y + z2);
        z2 = flushDenormal(b2 * x - a2 * y);
        return y;
    }

private:
    float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
    float z1 = 0.f, z2 = 0.f;
    float lastSr = 0.f, lastFreq = 0.f;
};

// Biquad RBJ: peaking dan shelf. Dipakai oleh Tone (bass / mid / treble).
class Biquad {
public:
    void reset() { z1 = z2 = 0.f; }
    void setPeaking(float sr, float f, float q, float gainDb) {
        const float A = std::pow(10.f, gainDb / 40.f);
        const float w0 = 6.2831853f * std::min(f, sr * 0.45f) / sr;
        const float cw = std::cos(w0);
        const float alpha = std::sin(w0) / (2.f * q);
        norm(1.f + alpha * A, -2.f * cw, 1.f - alpha * A, 1.f + alpha / A, -2.f * cw,
             1.f - alpha / A);
    }
    void setLowShelf(float sr, float f, float gainDb) {
        const float A = std::pow(10.f, gainDb / 40.f);
        const float w0 = 6.2831853f * std::min(f, sr * 0.45f) / sr;
        const float cw = std::cos(w0);
        const float beta = 2.f * std::sqrt(A) * (std::sin(w0) * 0.70710678f);
        norm(A * ((A + 1.f) - (A - 1.f) * cw + beta), 2.f * A * ((A - 1.f) - (A + 1.f) * cw),
             A * ((A + 1.f) - (A - 1.f) * cw - beta), (A + 1.f) + (A - 1.f) * cw + beta,
             -2.f * ((A - 1.f) + (A + 1.f) * cw), (A + 1.f) + (A - 1.f) * cw - beta);
    }
    void setHighShelf(float sr, float f, float gainDb) {
        const float A = std::pow(10.f, gainDb / 40.f);
        const float w0 = 6.2831853f * std::min(f, sr * 0.45f) / sr;
        const float cw = std::cos(w0);
        const float beta = 2.f * std::sqrt(A) * (std::sin(w0) * 0.70710678f);
        norm(A * ((A + 1.f) + (A - 1.f) * cw + beta), -2.f * A * ((A - 1.f) + (A + 1.f) * cw),
             A * ((A + 1.f) + (A - 1.f) * cw - beta), (A + 1.f) - (A - 1.f) * cw + beta,
             2.f * ((A - 1.f) - (A + 1.f) * cw), (A + 1.f) - (A - 1.f) * cw - beta);
    }
    float process(float x) {
        const float y = b0 * x + z1;
        z1 = flushDenormal(b1 * x - a1 * y + z2);
        z2 = flushDenormal(b2 * x - a2 * y);
        return y;
    }

private:
    void norm(float nb0, float nb1, float nb2, float na0, float na1, float na2) {
        const float inv = 1.f / na0;
        b0 = nb0 * inv;
        b1 = nb1 * inv;
        b2 = nb2 * inv;
        a1 = na1 * inv;
        a2 = na2 * inv;
    }
    float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
    float z1 = 0.f, z2 = 0.f;
};

// Tone control 3 band per kanal: bass (shelf 120 Hz), mid (peaking 1 kHz), treble (shelf 6 kHz).
class Tone {
public:
    void reset() {
        for (int i = 0; i < 3; ++i) {
            bl[i].reset();
            br[i].reset();
        }
    }
    // Koefisien hanya dihitung ulang kalau nilainya berubah.
    void setup(float sr, float bassDb, float midDb, float trebDb) {
        if (sr == lastSr && bassDb == lastB && midDb == lastM && trebDb == lastT) return;
        lastSr = sr;
        lastB = bassDb;
        lastM = midDb;
        lastT = trebDb;
        for (Biquad* b : {&bl[0], &br[0]}) b->setLowShelf(sr, 120.f, bassDb);
        for (Biquad* b : {&bl[1], &br[1]}) b->setPeaking(sr, 1000.f, 0.8f, midDb);
        for (Biquad* b : {&bl[2], &br[2]}) b->setHighShelf(sr, 6000.f, trebDb);
    }
    float processL(float x) { return bl[2].process(bl[1].process(bl[0].process(x))); }
    float processR(float x) { return br[2].process(br[1].process(br[0].process(x))); }

private:
    Biquad bl[3], br[3];
    float lastSr = 0.f, lastB = 1e9f, lastM = 1e9f, lastT = 1e9f;
};

// Equalizer 5 pita stereo (untuk master): shelf 80 Hz, peaking 250 Hz / 1 kHz / 4 kHz, shelf 10 kHz.
// Rentang tiap pita diatur dari luar (-12..+12 dB). Frekuensi tetap.
class Eq5 {
public:
    static constexpr int kBands = 5;

    void reset() {
        for (int i = 0; i < kBands; ++i) {
            bl[i].reset();
            br[i].reset();
        }
    }

    // Koefisien hanya dihitung ulang kalau nilainya berubah.
    void setup(float sr, const float* gDb) {
        bool same = (sr == lastSr);
        for (int i = 0; i < kBands && same; ++i) same = (gDb[i] == last[i]);
        if (same) return;
        lastSr = sr;
        for (int i = 0; i < kBands; ++i) last[i] = gDb[i];
        for (int ch = 0; ch < 2; ++ch) {
            Biquad* b = ch == 0 ? bl : br;
            b[0].setLowShelf(sr, 80.f, gDb[0]);
            b[1].setPeaking(sr, 250.f, 1.0f, gDb[1]);
            b[2].setPeaking(sr, 1000.f, 1.0f, gDb[2]);
            b[3].setPeaking(sr, 4000.f, 1.0f, gDb[3]);
            b[4].setHighShelf(sr, 10000.f, gDb[4]);
        }
    }

    void process(float& l, float& r) {
        for (int i = 0; i < kBands; ++i) {
            l = bl[i].process(l);
            r = br[i].process(r);
        }
    }

private:
    Biquad bl[kBands], br[kBands];
    float lastSr = 0.f;
    float last[kBands] = {1e9f, 1e9f, 1e9f, 1e9f, 1e9f};
};

// Equalizer vokal 10 pita stereo (khusus strip A mic), menjangkau 20 Hz - 20 kHz:
// shelf rendah 40 Hz (mewakili 20-40 Hz), peaking 80/160/315/630/1250/2500/5000/10000 Hz
// (Q 1.4, jarak sekitar satu oktaf), shelf tinggi 14 kHz (mewakili 14-20 kHz).
class VocalEq {
public:
    static constexpr int kBands = 10;

    void reset() {
        for (int i = 0; i < kBands; ++i) {
            bl[i].reset();
            br[i].reset();
        }
    }

    // Koefisien hanya dihitung ulang kalau nilainya berubah.
    void setup(float sr, const float* gDb) {
        bool same = (sr == lastSr);
        for (int i = 0; i < kBands && same; ++i) same = (gDb[i] == last[i]);
        if (same) return;
        lastSr = sr;
        for (int i = 0; i < kBands; ++i) last[i] = gDb[i];
        static const float kFreq[kBands] = {40.f,   80.f,   160.f,  315.f,   630.f,
                                            1250.f, 2500.f, 5000.f, 10000.f, 14000.f};
        for (int ch = 0; ch < 2; ++ch) {
            Biquad* b = ch == 0 ? bl : br;
            b[0].setLowShelf(sr, kFreq[0], gDb[0]);
            for (int i = 1; i < kBands - 1; ++i) b[i].setPeaking(sr, kFreq[i], 1.4f, gDb[i]);
            b[kBands - 1].setHighShelf(sr, kFreq[kBands - 1], gDb[kBands - 1]);
        }
    }

    void process(float& l, float& r) {
        for (int i = 0; i < kBands; ++i) {
            l = bl[i].process(l);
            r = br[i].process(r);
        }
    }
    float processMono(float l) {
        for (int i = 0; i < kBands; ++i) l = bl[i].process(l);
        return l;
    }

private:
    Biquad bl[kBands], br[kBands];
    float lastSr = 0.f;
    float last[kBands] = {1e9f, 1e9f, 1e9f, 1e9f, 1e9f, 1e9f, 1e9f, 1e9f, 1e9f, 1e9f};
};

// Noise gate (terhubung L/R): membuka cepat, menahan 60 ms, menutup sesuai release.
// Saat tertutup sinyal ditekan ~45 dB (bukan senyap total, supaya tidak terdengar patah).
class Gate {
public:
    void prepare(float sr, float releaseMs) {
        atk = std::exp(-1.f / (0.001f * sr));
        rel = std::exp(-1.f / (std::max(releaseMs, 5.f) * 0.001f * sr));
        envRel = std::exp(-1.f / (0.030f * sr));
        holdMax = 0.060f * sr;
    }
    void reset() {
        env = 0.f;
        gain = 1.f;
        hold = 0.f;
        open = true;
    }
    // lvl = level puncak saat ini (linear), thr = ambang buka (linear). Mengembalikan gain.
    float process(float lvl, float thr) {
        env = (lvl > env) ? lvl : envRel * env + (1.f - envRel) * lvl;
        if (env > thr) {
            open = true;
            hold = holdMax;
        } else if (env < thr * 0.7f) {
            if (hold > 0.f) hold -= 1.f;
            else open = false;
        }
        const float target = open ? 1.f : kFloor;
        gain = (target > gain) ? atk * gain + (1.f - atk) * target
                               : rel * gain + (1.f - rel) * target;
        return gain;
    }

private:
    static constexpr float kFloor = 0.0056f;  // ~ -45 dB
    float atk = 0.f, rel = 0.f, envRel = 0.f, holdMax = 0.f;
    float env = 0.f, gain = 1.f, hold = 0.f;
    bool open = true;
};

class Reverb {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        const float k = sr / 44100.f;
        static const int combTune[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int apTune[4] = {556, 441, 341, 225};
        const int spread = 23;
        for (int i = 0; i < 8; ++i) {
            combL[i].alloc(scaled(combTune[i], k));
            combR[i].alloc(scaled(combTune[i] + spread, k));
        }
        for (int i = 0; i < 4; ++i) {
            apL[i].alloc(scaled(apTune[i], k));
            apR[i].alloc(scaled(apTune[i] + spread, k));
        }
        pre.assign(static_cast<size_t>(sr * 0.12f) + 2, 0.f);
        preIdx = 0;
        lcState = 0.f;
        setParams(60.f, 50.f, 15.f, 100.f, 200.f, 20.f);
    }

    void reset() {
        for (int i = 0; i < 8; ++i) {
            combL[i].clear();
            combR[i].clear();
        }
        for (int i = 0; i < 4; ++i) {
            apL[i].clear();
            apR[i].clear();
        }
        std::fill(pre.begin(), pre.end(), 0.f);
        lcState = 0.f;
    }

    // Semua dalam persen (0-100) kecuali preMs (ms) dan lowcutHz (Hz).
    void setParams(float sizePct, float dampPct, float preMs, float widthPct, float lowcutHz,
                   float mixPct) {
        const float size = std::min(std::max(sizePct, 0.f), 100.f) * 0.01f;
        const float damp = std::min(std::max(dampPct, 0.f), 100.f) * 0.01f;
        const float width = std::min(std::max(widthPct, 0.f), 100.f) * 0.01f;
        feedback = 0.7f + 0.28f * size;
        damp1 = damp * 0.4f;
        damp2 = 1.f - damp1;
        wet1 = width * 0.5f + 0.5f;
        wet2 = (1.f - width) * 0.5f;
        mix = std::min(std::max(mixPct, 0.f), 100.f) * 0.01f;
        const float maxPre = pre.empty() ? 0.f : static_cast<float>(pre.size() - 1);
        preDelay = static_cast<size_t>(std::min(std::max(preMs * 0.001f * sr, 0.f), maxPre));
        const float f = std::min(std::max(lowcutHz, 10.f), sr * 0.4f);
        lcA = 1.f - std::exp(-6.2831853f * f / sr);
    }

    // Menambahkan ekor reverb ke (l, r). Panggil hanya dengan jalur stereo.
    void process(float& l, float& r) {
        if (pre.empty()) return;
        float in = (l + r) * 0.5f;
        lcState += lcA * (in - lcState);  // low-cut: buang bass dari masukan reverb
        in -= lcState;
        pre[preIdx] = in;
        size_t rd = preIdx + pre.size() - preDelay;
        if (rd >= pre.size()) rd -= pre.size();
        const float d = pre[rd];
        if (++preIdx >= pre.size()) preIdx = 0;

        const float x = d * 0.03f;
        float oL = 0.f, oR = 0.f;
        for (int i = 0; i < 8; ++i) {
            oL += combL[i].run(x, feedback, damp1, damp2);
            oR += combR[i].run(x, feedback, damp1, damp2);
        }
        for (int i = 0; i < 4; ++i) {
            oL = apL[i].run(oL);
            oR = apR[i].run(oR);
        }
        l += (oL * wet1 + oR * wet2) * mix;
        r += (oR * wet1 + oL * wet2) * mix;
    }

private:
    static size_t scaled(int tune, float k) {
        return static_cast<size_t>(std::max(1.f, std::floor(static_cast<float>(tune) * k + 0.5f)));
    }

    struct Comb {
        std::vector<float> buf;
        size_t idx = 0;
        float store = 0.f;
        void alloc(size_t n) {
            buf.assign(n, 0.f);
            idx = 0;
            store = 0.f;
        }
        void clear() {
            std::fill(buf.begin(), buf.end(), 0.f);
            idx = 0;
            store = 0.f;
        }
        float run(float in, float fb, float d1, float d2) {
            const float out = buf[idx];
            store = flushDenormal(out * d2 + store * d1);
            buf[idx] = flushDenormal(in + store * fb);
            if (++idx >= buf.size()) idx = 0;
            return out;
        }
    };
    struct Allpass {
        std::vector<float> buf;
        size_t idx = 0;
        void alloc(size_t n) {
            buf.assign(n, 0.f);
            idx = 0;
        }
        void clear() {
            std::fill(buf.begin(), buf.end(), 0.f);
            idx = 0;
        }
        float run(float in) {
            const float b = buf[idx];
            const float out = b - in;
            buf[idx] = flushDenormal(in + b * 0.5f);
            if (++idx >= buf.size()) idx = 0;
            return out;
        }
    };

    Comb combL[8], combR[8];
    Allpass apL[4], apR[4];
    std::vector<float> pre;
    size_t preIdx = 0, preDelay = 0;
    float sr = 48000.f;
    float feedback = 0.84f, damp1 = 0.2f, damp2 = 0.8f;
    float wet1 = 1.f, wet2 = 0.f, mix = 0.2f;
    float lcA = 0.f, lcState = 0.f;
};
