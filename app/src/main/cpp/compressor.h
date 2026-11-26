#pragma once
// DSP inti: compressor (RMS detector, soft knee ala "OverEasy") + limiter (peak),
// plus rantai stereo (opsi link L/R). Semua gain dihitung dalam dB, dihaluskan
// dengan attack/release satu kutub.

#include <algorithm>
#include <cmath>

#include "dsp_base.h"
#include "effects.h"
#include "multiband.h"

class Compressor {
public:
    float gr = 0.f;  // gain reduction saat ini dalam dB (<= 0)

    void reset() {
        ms = 0.f;
        gr = 0.f;
    }

    void prepare(float sampleRate, const CompParams& p) {
        atk = std::exp(-1.f / (std::max(p.attackMs, 0.05f) * 0.001f * sampleRate));
        rel = std::exp(-1.f / (std::max(p.releaseMs, 1.f) * 0.001f * sampleRate));
        rmsCoef = std::exp(-1.f / (0.005f * sampleRate));  // jendela RMS ~5 ms
    }

    // Mengembalikan gain linear (sudah termasuk makeup) untuk sampel x.
    float process(float x, const CompParams& p) {
        ms = rmsCoef * ms + (1.f - rmsCoef) * x * x;
        const float levelDb = 10.f * std::log10(ms + 1e-12f);
        const float over = levelDb - p.thresholdDb;
        const float slope = 1.f / std::max(p.ratio, 1.f) - 1.f;  // <= 0
        const float knee = p.kneeDb;

        float target;  // perubahan gain yang diinginkan (dB, <= 0)
        if (knee < 0.1f) {
            target = over > 0.f ? slope * over : 0.f;  // hard knee
        } else {
            const float half = 0.5f * knee;
            if (over <= -half) {
                target = 0.f;
            } else if (over >= half) {
                target = slope * over;
            } else {
                const float t = over + half;
                target = slope * t * t / (2.f * knee);  // soft knee
            }
        }

        gr = (target < gr) ? atk * gr + (1.f - atk) * target
                           : rel * gr + (1.f - rel) * target;
        return dbToLin(gr + p.makeupDb);
    }

private:
    float ms = 0.f, atk = 0.f, rel = 0.f, rmsCoef = 0.f;
};

class Limiter {
public:
    float gr = 0.f;  // dB (<= 0)

    void reset() { gr = 0.f; }

    void prepare(float sampleRate, float ceilingDb) {
        ceilDb = ceilingDb;
        ceilLin = dbToLin(ceilingDb);
        atk = std::exp(-1.f / (0.0005f * sampleRate));  // 0.5 ms
        rel = std::exp(-1.f / (0.080f * sampleRate));   // 80 ms
    }

    // Memperbarui detektor dan mengembalikan gain linear (tanpa clip).
    float gain(float x) {
        const float a = std::fabs(x) + 1e-9f;
        const float db = 20.f * std::log10(a);
        const float target = db > ceilDb ? ceilDb - db : 0.f;
        gr = (target < gr) ? atk * gr + (1.f - atk) * target
                           : rel * gr + (1.f - rel) * target;
        return dbToLin(gr);
    }

    // Pengaman terakhir: tanpa lookahead, puncak sangat cepat bisa lolos sesaat,
    // jadi dipotong keras di ceiling.
    float clip(float y) const { return std::max(-ceilLin, std::min(ceilLin, y)); }

    float process(float x) { return clip(x * gain(x)); }

private:
    float ceilDb = -1.f, ceilLin = 0.891f, atk = 0.f, rel = 0.f;
};

// Limiter master dengan lookahead (~1,5 ms): gain sudah turun sebelum puncak tiba, jadi tidak
// ada puncak yang lolos lalu terpotong (penyebab suara pecah). Menambah latensi ~1,5 ms.
class LookaheadLimiter {
public:
    float gr = 0.f;  // dB (<= 0), untuk meter

    void reset() {
        for (int i = 0; i < kMax; ++i) dl[i] = dr[i] = 0.f, tg[i] = 1.f;
        idx = 0;
        gs = 1.f;
        gr = 0.f;
    }

    void prepare(float sampleRate, float ceilingDb) {
        ceilLin = dbToLin(ceilingDb);
        la = std::max(8, std::min(kMax - 1, static_cast<int>(0.0015f * sampleRate)));
        atk = std::exp(-3.f / static_cast<float>(la));   // capai ~95% dalam `la` sampel
        rel = std::exp(-1.f / (0.100f * sampleRate));    // 100 ms
    }

    // enabled=false: gain 1 (hanya tertunda, supaya latensi tetap sama).
    void process(float& l, float& r, bool enabled) {
        const float peak = std::max(std::fabs(l), std::fabs(r));
        const float t = (enabled && peak > ceilLin) ? ceilLin / peak : 1.f;
        dl[idx] = l;
        dr[idx] = r;
        tg[idx] = t;
        float m = 1.f;  // gain minimum di seluruh jendela lookahead
        for (int k = 0; k <= la; ++k) {
            int j = idx - k;
            if (j < 0) j += kMax;
            m = std::min(m, tg[j]);
        }
        gs = (m < gs) ? atk * gs + (1.f - atk) * m : rel * gs + (1.f - rel) * m;
        int rd = idx - la;
        if (rd < 0) rd += kMax;
        l = dl[rd] * gs;
        r = dr[rd] * gs;
        if (enabled) {  // pengaman terakhir untuk sisa kecil
            l = std::max(-ceilLin, std::min(ceilLin, l));
            r = std::max(-ceilLin, std::min(ceilLin, r));
        }
        if (++idx >= kMax) idx = 0;
        gr = 20.f * std::log10(gs + 1e-9f);
    }

private:
    static constexpr int kMax = 256;
    float dl[kMax] = {}, dr[kMax] = {}, tg[kMax] = {};
    int idx = 0, la = 72;
    float ceilLin = 0.891f, atk = 0.f, rel = 0.f, gs = 1.f;
};

struct ProcParams {
    CompParams comp;
    float ceilDb = -1.f;
    bool limOn = true;
    bool link = true;     // L/R berbagi gain reduction (ambil yang terbesar)
    float outGain = 1.f;  // linear
    bool revOn = false;   // reverb di antara compressor dan limiter
    MbParams mb;          // compressor multiband + auto (hanya strip A mic); mb.on = false -> dBFS biasa
};

// Rantai compressor -> limiter -> output gain untuk satu frame (mono atau stereo).
class StereoChain {
public:
    void reset() {
        cL.reset();
        cR.reset();
        lL.reset();
        lR.reset();
        mbL.reset();
        mbR.reset();
        mbWasOn = false;
        xf = 0.f;
    }

    void prepare(float sampleRate, const ProcParams& p) {
        cL.prepare(sampleRate, p.comp);
        cR.prepare(sampleRate, p.comp);
        if (p.mb.on && !mbWasOn) {
            // Pindah ke mode multiband: bersihkan state filter dan adaptasi.
            mbL.reset();
            mbR.reset();
        }
        if (p.mb.on || xf > 0.f) {  // tetap disiapkan selama crossfade keluar
            mbL.prepare(sampleRate, p.comp, p.mb);
            mbR.prepare(sampleRate, p.comp, p.mb);
        }
        mbWasOn = p.mb.on;
        xfStep = 1.f / std::max(1.f, 0.015f * sampleRate);  // crossfade 15 ms antar mode
        lL.prepare(sampleRate, p.ceilDb);
        lR.prepare(sampleRate, p.ceilDb);
    }

    // stereo=false: hanya l yang dipakai, yr dibuat sama dengan yl.
    // reduction: gain reduction total (compressor + limiter) dalam dB, positif.
    void process(float l, float r, bool stereo, const ProcParams& p,
                 float& yl, float& yr, float& reduction) {
        // Crossfade 15 ms saat berpindah mode dBFS <-> multiband, supaya tidak ada klik
        // (kedua jalur punya gain dan state berbeda).
        const float xfTarget = p.mb.on ? 1.f : 0.f;
        if (xf < xfTarget) xf = std::min(xfTarget, xf + xfStep);
        else if (xf > xfTarget) xf = std::max(xfTarget, xf - xfStep);

        float red = 0.f;
        yl = yr = 0.f;
        if (xf < 1.f) {  // jalur dBFS biasa
            float gLeft = cL.process(l, p.comp);
            float gRight = 1.f;
            float redS = -cL.gr;
            if (stereo) {
                gRight = cR.process(r, p.comp);
                redS = std::max(redS, -cR.gr);
                if (p.link) {
                    const float g = std::min(gLeft, gRight);
                    gLeft = gRight = g;
                }
            }
            const float w = 1.f - xf;
            yl = l * gLeft * w;
            yr = (stereo ? r * gRight : l * gLeft) * w;
            red = redS * w;
        }
        if (xf > 0.f) {  // jalur multiband + auto (mono; R hanya bila jalur stereo, tanpa link)
            const float ml = mbL.process(l, p.comp, p.mb);
            float redM = -mbL.gr;
            float mr = ml;
            if (stereo) {
                mr = mbR.process(r, p.comp, p.mb);
                redM = std::max(redM, -mbR.gr);
            }
            yl += ml * xf;
            yr += mr * xf;
            red += redM * xf;
        }

        if (stereo && rev != nullptr && p.revOn) rev->process(yl, yr);

        if (p.limOn) {
            float limLeft = lL.gain(yl);
            float limRight = limLeft;
            float limRed = -lL.gr;
            if (stereo) {
                limRight = lR.gain(yr);
                limRed = std::max(limRed, -lR.gr);
                if (p.link) {
                    const float g = std::min(limLeft, limRight);
                    limLeft = limRight = g;
                }
            }
            yl = lL.clip(yl * limLeft);
            yr = stereo ? lR.clip(yr * limRight) : yl;
            red += limRed;
        }

        yl *= p.outGain;
        yr = stereo ? yr * p.outGain : yl;
        reduction = red;
    }

    Reverb* rev = nullptr;  // opsional; dipakai hanya di jalur stereo

private:
    Compressor cL, cR;
    MultibandComp mbL, mbR;
    bool mbWasOn = false;
    float xf = 0.f, xfStep = 0.01f;  // posisi crossfade: 0 = dBFS biasa, 1 = multiband
    Limiter lL, lR;
};
