#pragma once
// Compressor multiband + auto-level untuk VOKAL NYANYI (dipakai strip A mic, 1 channel).
//
// Alur satu kanal:
//   in -> crossover LR4 3 pita (rendah | tengah | tinggi) -> compressor per pita (dBFS RMS,
//   soft knee) -> jumlahkan -> makeup (manual trim + otomatis) -> out
//
// Pembagian pita untuk vokal:
//   rendah  < lowHz  (default 200 Hz)  : bobot, proximity, plosive sisa
//   tengah  lowHz..highHz              : badan dan kejelasan suara
//   tinggi  > highHz (default 4,5 kHz) : sibilance "s/sy" dan udara -> bertindak seperti de-esser
//
// "Auto": threshold tiap pita mengikuti level rata-rata pita itu sendiri (hanya dihitung saat
// ada suara di atas noise floor), jadi tidak perlu diubah saat jarak mic atau kerasnya suara
// berubah. Makeup otomatis menarik level rata-rata hasil ke target RMS. Saat tidak ada suara
// (di bawah noise floor) semua nilai adaptasi DIBEKUKAN, sehingga desis tidak ikut diangkat.

#include <algorithm>
#include <cmath>

#include "dsp_base.h"  // dbToLin, CompParams
#include "effects.h"   // HighPass, flushDenormal

// Low-pass orde-2 Butterworth, pasangan HighPass di effects.h.
class LowPass {
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
        b0 = (1.f - cw) * 0.5f / a0;
        b1 = (1.f - cw) / a0;
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

// Parameter mode multiband. Dikirim dari engine tiap blok (nilai dari slider).
struct MbParams {
    bool on = false;
    bool autoOn = true;
    float targetDb = -18.f;    // target RMS rata-rata hasil (dBFS), hanya bila autoOn
    float amount = 50.f;       // % kekuatan kompresi (hanya bila autoOn)
    float sibilance = 50.f;    // % kekuatan kontrol sibilance di pita tinggi (hanya bila autoOn)
    float lowHz = 200.f;
    float highHz = 4500.f;
    float noiseFloorDb = -50.f;  // di bawah ini dianggap diam: adaptasi dibekukan
};

// Satu compressor pita: detektor RMS -> gain computer soft knee -> attack/release dalam dB.
class BandComp {
public:
    float gr = 0.f;  // dB (<= 0)

    void reset() {
        ms = 0.f;
        gr = 0.f;
    }

    void prepare(float sr, float rmsMs, float attackMs, float releaseMs) {
        rmsCoef = std::exp(-1.f / (std::max(rmsMs, 0.2f) * 0.001f * sr));
        atk = std::exp(-1.f / (std::max(attackMs, 0.05f) * 0.001f * sr));
        rel = std::exp(-1.f / (std::max(releaseMs, 1.f) * 0.001f * sr));
    }

    // Mengembalikan gain linear untuk sampel pita x.
    float process(float x, float thresholdDb, float ratio, float kneeDb) {
        ms = rmsCoef * ms + (1.f - rmsCoef) * x * x;
        ms = flushDenormal(ms);
        const float levelDb = 10.f * std::log10(ms + 1e-12f);
        lastLevelDb = levelDb;
        const float over = levelDb - thresholdDb;
        const float slope = 1.f / std::max(ratio, 1.f) - 1.f;  // <= 0

        float target;
        if (kneeDb < 0.1f) {
            target = over > 0.f ? slope * over : 0.f;
        } else {
            const float half = 0.5f * kneeDb;
            if (over <= -half) target = 0.f;
            else if (over >= half) target = slope * over;
            else {
                const float t = over + half;
                target = slope * t * t / (2.f * kneeDb);
            }
        }
        gr = (target < gr) ? atk * gr + (1.f - atk) * target
                           : rel * gr + (1.f - rel) * target;
        return dbToLin(gr);
    }

    float lastLevelDb = -120.f;

private:
    float ms = 0.f, rmsCoef = 0.f, atk = 0.f, rel = 0.f;
};

class MultibandComp {
public:
    static constexpr int kBands = 3;
    float gr = 0.f;  // gain reduction terdalam di antara pita (dB, <= 0), untuk meter

    void reset() {
        lp1a.reset(); lp1b.reset(); hp1a.reset(); hp1b.reset();
        lp2a.reset(); lp2b.reset(); hp2a.reset(); hp2b.reset();
        apLa.reset(); apLb.reset(); apHa.reset(); apHb.reset();
        for (auto& b : band) b.reset();
        gr = 0.f;
        fullMs = 0.f;
        for (int i = 0; i < kBands; ++i) avgMs[i] = 0.f;
        outAvgMs = 0.f;
        avgInit = false;
        activeSec = 0.f;
        autoMakeDb = 0.f;
        autoMakeLin = 1.f;
        ctr = 0;
        for (int i = 0; i < kBands; ++i) thrDb[i] = -30.f;
    }

    // Dipanggil tiap blok: koefisien filter dan waktu attack/release per pita.
    void prepare(float sr, const CompParams& c, const MbParams& m) {
        sampleRate = sr;
        outCoef = 1.f - std::exp(-1.f / (1.5f * sr));
        fullCoef = 1.f - std::exp(-1.f / (0.010f * sr));
        const float lo = std::min(std::max(m.lowHz, 60.f), 600.f);
        const float hi = std::min(std::max(m.highHz, 1500.f), 12000.f);
        lp1a.setup(sr, lo); lp1b.setup(sr, lo);
        hp1a.setup(sr, lo); hp1b.setup(sr, lo);
        lp2a.setup(sr, hi); lp2b.setup(sr, hi);
        hp2a.setup(sr, hi); hp2b.setup(sr, hi);
        // all-pass pencocok untuk pita rendah (LP2 + HP2 pada f tinggi), supaya jumlah
        // ketiga pita rata (fase pita rendah sama dengan pita lain)
        apLa.setup(sr, hi); apLb.setup(sr, hi);
        apHa.setup(sr, hi); apHb.setup(sr, hi);

        // Karakter tiap pita (turunan dari slider Attack/Release):
        //  rendah: lebih lambat, tengah: seperti slider, tinggi: cepat (de-esser).
        const float a = c.attackMs, r = c.releaseMs;
        band[0].prepare(sr, 10.f, std::max(a * 2.f, 5.f), r * 1.5f);
        band[1].prepare(sr, 5.f, a, r);
        band[2].prepare(sr, 2.f, std::max(a * 0.3f, 0.5f), std::max(r * 0.4f, 20.f));
    }

    // x = sampel masuk (setelah HPF/gate/tone), c = slider kompresor, m = parameter multiband.
    float process(float x, const CompParams& c, const MbParams& m) {
        // --- crossover LR4 3 pita ---
        const float low0 = lp1b.process(lp1a.process(x));
        const float rest = hp1b.process(hp1a.process(x));
        const float mid = lp2b.process(lp2a.process(rest));
        const float high = hp2b.process(hp2a.process(rest));
        // all-pass pencocok untuk pita rendah
        const float low = apLb.process(apLa.process(low0)) + apHb.process(apHa.process(low0));

        // --- deteksi aktivitas suara (broadband, ~10 ms) ---
        fullMs += fullCoef * (x * x - fullMs);
        const bool active = fullMs > noiseFloorLin(m.noiseFloorDb);

        // --- adaptasi (tiap kPeriod sampel) ---
        if (++ctr >= kPeriod) {
            ctr = 0;
            updateControl(c, m, active);
        }

        // --- kompresor per pita ---
        const float gL = band[0].process(low, thrDb[0], ratioFor(0, c), c.kneeDb);
        const float gM = band[1].process(mid, thrDb[1], ratioFor(1, c), c.kneeDb);
        const float gH = band[2].process(high, thrDb[2], ratioFor(2, c), c.kneeDb);
        const float y = low * gL + mid * gM + high * gH;

        gr = std::min(band[0].gr, std::min(band[1].gr, band[2].gr));

        // --- rata-rata hasil sebelum makeup (untuk auto makeup), hanya saat ada suara ---
        if (active) outAvgMs += outCoef * (y * y - outAvgMs);

        return y * autoMakeLin * manualLin(c.makeupDb);
    }

    float autoMakeupDb() const { return autoMakeDb; }

private:
    static constexpr int kPeriod = 32;

    static float noiseFloorLin(float db) { return std::exp(db * 0.23025851f); }  // 10^(db/10)
    static float manualLin(float db) { return dbToLin(db); }

    // Ratio per pita: tengah = slider, rendah lebih lembut, tinggi lebih tegas.
    static float ratioFor(int b, const CompParams& c) {
        if (b == 0) return std::max(1.5f, c.ratio * 0.6f);
        if (b == 2) return std::min(20.f, c.ratio * 1.5f);
        return c.ratio;
    }

    void updateControl(const CompParams& c, const MbParams& m, bool active) {
        const float dt = kPeriod / sampleRate;

        // threshold tiap pita
        if (m.autoOn) {
            if (active) {
                // rata-rata level pita: konstanta waktu naik dari 0,3 s ke 2 s seiring lama aktif
                activeSec += dt;
                const float tau = 0.3f + std::min(1.7f, activeSec * 0.5f);
                const float k = 1.f - std::exp(-dt / tau);
                for (int i = 0; i < kBands; ++i) {
                    const float lv = std::pow(10.f, band[i].lastLevelDb * 0.1f);
                    if (!avgInit) avgMs[i] = lv;
                    else avgMs[i] += k * (lv - avgMs[i]);
                }
                avgInit = true;
            }
            const float a = std::min(std::max(m.amount, 0.f), 100.f) * 0.01f;
            const float s = std::min(std::max(m.sibilance, 0.f), 100.f) * 0.01f;
            // offset threshold di atas level rata-rata pita (dB): makin besar amount makin rendah
            const float offMid = 6.f - 8.f * a;
            const float off[kBands] = {offMid + 1.f, offMid, 5.f - 9.f * s};
            for (int i = 0; i < kBands; ++i) {
                const float avgDb = avgInit ? 10.f * std::log10(avgMs[i] + 1e-12f) : -40.f;
                thrDb[i] = avgDb + off[i];
            }
        } else {
            // manual: tengah = slider, rendah sedikit lebih tinggi, tinggi sedikit lebih rendah
            thrDb[0] = c.thresholdDb + 2.f;
            thrDb[1] = c.thresholdDb;
            thrDb[2] = c.thresholdDb - 2.f;
        }

        // auto makeup: tarik rata-rata hasil ke target; hanya bergerak saat ada suara
        if (m.autoOn && active && avgInit && outAvgMs > 1e-12f) {
            const float outDb = 10.f * std::log10(outAvgMs + 1e-12f);
            const float need = std::min(std::max(m.targetDb - outDb, -6.f), 18.f);
            const float tau = (need > autoMakeDb) ? 1.2f : 0.3f;  // naik pelan, turun lebih cepat
            autoMakeDb += (1.f - std::exp(-dt / tau)) * (need - autoMakeDb);
        } else if (!m.autoOn) {
            autoMakeDb += (1.f - std::exp(-dt / 0.3f)) * (0.f - autoMakeDb);  // kembali ke 0
        }
        autoMakeLin = dbToLin(autoMakeDb);
    }

    // crossover
    HighPass hp1a, hp1b, hp2a, hp2b;
    LowPass lp1a, lp1b, lp2a, lp2b;
    // all-pass pencocok pita rendah
    LowPass apLa, apLb;
    HighPass apHa, apHb;

    BandComp band[kBands];
    float thrDb[kBands] = {-30.f, -30.f, -30.f};
    float avgMs[kBands] = {0.f, 0.f, 0.f};
    float outAvgMs = 0.f;
    float fullMs = 0.f;
    bool avgInit = false;
    float activeSec = 0.f;
    float autoMakeDb = 0.f, autoMakeLin = 1.f;
    float outCoef = 1.f - std::exp(-1.f / (1.5f * 48000.f));
    float fullCoef = 1.f - std::exp(-1.f / (0.010f * 48000.f));
    float sampleRate = 48000.f;
    int ctr = 0;
};
