#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
#include "../../app/src/main/cpp/multiband.h"

static const float SR = 48000.f;
static float rmsDb(const std::vector<float>& v, size_t a, size_t b) {
    double s = 0; for (size_t i = a; i < b; ++i) s += (double)v[i] * v[i];
    return 10.f * std::log10(s / (b - a) + 1e-20);
}

// Tes 1: respons rata saat kompresi mati (ratio 1, threshold tinggi), manual mode
static void testFlat() {
    printf("[1] Respons frekuensi (kompresi netral) -- harus rata\n");
    float worst = 0;
    for (float f : {50.f, 100.f, 180.f, 200.f, 220.f, 500.f, 1000.f, 3000.f, 4500.f, 5000.f, 8000.f, 14000.f}) {
        MultibandComp mb; mb.reset();
        CompParams c; c.thresholdDb = 20; c.ratio = 1; c.kneeDb = 0; c.makeupDb = 0;
        MbParams m; m.on = true; m.autoOn = false;
        mb.prepare(SR, c, m);
        std::vector<float> in, out;
        for (int i = 0; i < 48000; ++i) {
            float x = 0.25f * std::sin(6.2831853f * f * i / SR);
            in.push_back(x); out.push_back(mb.process(x, c, m));
        }
        float d = rmsDb(out, 24000, 48000) - rmsDb(in, 24000, 48000);
        worst = std::max(worst, std::fabs(d));
        printf("   %6.0f Hz : %+.2f dB\n", f, d);
    }
    printf("   -> deviasi terbesar %.2f dB %s\n", worst, worst < 1.0f ? "OK" : "GAGAL");
}

// sinyal "vokal": harmonik 220 Hz dengan modulasi amplitudo suku kata
static std::vector<float> voice(float lvlDb, float sec, unsigned seed = 1) {
    std::mt19937 g(seed); std::normal_distribution<float> n(0, 1);
    std::vector<float> v;
    int N = (int)(sec * SR);
    float amp = std::pow(10.f, lvlDb / 20.f);
    for (int i = 0; i < N; ++i) {
        float t = i / SR;
        float env = 0.55f + 0.45f * std::sin(6.2831853f * 3.f * t);  // ~3 suku kata/dtk
        float s = 0;
        for (int h = 1; h <= 12; ++h) s += std::sin(6.2831853f * 220.f * h * t) / h;
        v.push_back(amp * env * (0.45f * s + 0.01f * n(g)));
    }
    return v;
}

static std::vector<float> run(const std::vector<float>& in, MultibandComp& mb, const CompParams& c, const MbParams& m) {
    std::vector<float> out; out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (i % 256 == 0) mb.prepare(SR, c, m);
        out.push_back(mb.process(in[i], c, m));
    }
    return out;
}

static void testAutoLevel() {
    printf("[2] Auto level: keluaran harus mendekati target -18 dBFS RMS di berbagai level masuk\n");
    CompParams c; c.thresholdDb = -20; c.ratio = 3; c.attackMs = 10; c.releaseMs = 150; c.kneeDb = 6;
    MbParams m; m.on = true; m.autoOn = true; m.targetDb = -18; m.amount = 50; m.sibilance = 50;
    for (float lvl : {-40.f, -30.f, -22.f, -14.f, -8.f}) {
        MultibandComp mb; mb.reset();
        auto in = voice(lvl, 12.f);
        auto out = run(in, mb, c, m);
        printf("   masuk %6.1f dB RMS -> keluar %6.1f dB RMS (makeup %+.1f dB)\n",
               rmsDb(in, 6 * 48000, 12 * 48000), rmsDb(out, 6 * 48000, 12 * 48000), mb.autoMakeupDb());
    }
}

static void testNoiseFreeze() {
    printf("[3] Noise floor: desis -70 dBFS tidak boleh diangkat; setelah suara berhenti makeup beku\n");
    CompParams c; MbParams m; m.on = true; m.autoOn = true; m.noiseFloorDb = -50;
    MultibandComp mb; mb.reset();
    auto v = voice(-26.f, 6.f);
    auto o1 = run(v, mb, c, m);
    float mk = mb.autoMakeupDb();
    std::mt19937 g(5); std::normal_distribution<float> n(0, 1);
    std::vector<float> hiss; float a = std::pow(10.f, -70.f / 20.f);
    for (int i = 0; i < 6 * 48000; ++i) hiss.push_back(a * n(g));
    auto o2 = run(hiss, mb, c, m);
    printf("   makeup setelah suara %+.2f dB, setelah 6 dtk desis %+.2f dB (harus sama)\n", mk, mb.autoMakeupDb());
    printf("   desis masuk %.1f dB -> keluar %.1f dB\n", rmsDb(hiss, 48000, 288000), rmsDb(o2, 48000, 288000));
}

static void testSibilance() {
    printf("[4] De-ess: 's' (7 kHz) relatif terhadap vokal -- selisih keluar-masuk makin negatif = makin ditekan\n");
    for (float sib : {0.f, 50.f, 100.f}) {
        CompParams c; c.ratio = 4; c.attackMs = 5; c.releaseMs = 120;
        MbParams m; m.on = true; m.autoOn = true; m.sibilance = sib; m.targetDb = -18;
        MultibandComp mb; mb.reset();
        std::vector<float> in = voice(-22.f, 10.f);
        std::mt19937 g(9); std::normal_distribution<float> n(0, 1);
        // desis HF latar (noise tilt) di vokal: sifat vokal nyata punya sedikit energi >4,5 kHz
        float prev = 0;
        for (size_t i = 0; i < in.size(); ++i) { float w = n(g); in[i] += 0.004f * (w - prev); prev = w; }
        // 's' tiap detik, 100 ms, ~+10 dB di atas level vokal (tiap 's' di jendela tanpa vokal berat)
        float sAmp = 0.10f;
        for (int k = 3; k < 10; ++k)
            for (int i = 0; i < 4800; ++i) {
                size_t idx = k * 48000 + 20000 + i;
                in[idx] += sAmp * std::sin(6.2831853f * 7000.f * idx / SR);
            }
        auto out = run(in, mb, c, m);
        double dS = 0, dV = 0; int cnt = 0;
        for (int k = 5; k < 10; ++k) {
            size_t sa = k * 48000 + 20000 + 1200, sb = sa + 3000;   // segmen 's'
            size_t va = k * 48000 + 5000,  vb = va + 8000;          // vokal tanpa 's'
            dS += rmsDb(out, sa, sb) - rmsDb(in, sa, sb);
            dV += rmsDb(out, va, vb) - rmsDb(in, va, vb);
            ++cnt;
        }
        printf("   sibilance %3.0f%% : gain efektif 's' %+.1f dB, vokal %+.1f dB -> 's' lebih ditekan %.1f dB dari vokal\n",
               sib, dS / cnt, dV / cnt, (dV - dS) / cnt);
    }
}

static void testNaNAndSpeed() {
    printf("[5] Stabilitas dan CPU\n");
    CompParams c; MbParams m; m.on = true; m.autoOn = true;
    MultibandComp mb; mb.reset();
    auto v = voice(-20.f, 10.f);
    auto t0 = std::chrono::steady_clock::now();
    auto o = run(v, mb, c, m);
    auto t1 = std::chrono::steady_clock::now();
    bool bad = false; float pk = 0;
    for (float x : o) { if (!std::isfinite(x)) bad = true; pk = std::max(pk, std::fabs(x)); }
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    printf("   NaN/Inf: %s | puncak keluar %.2f | 10 dtk audio diproses %.1f ms (%.2f%% 1 core host)\n",
           bad ? "ADA (GAGAL)" : "tidak ada", pk, ms, ms / 100.0);
    // diam total + denormal
    std::vector<float> z(48000 * 3, 0.f);
    auto oz = run(z, mb, c, m);
    bool bad2 = false; for (float x : oz) if (!std::isfinite(x)) bad2 = true;
    printf("   input nol 3 dtk: %s\n", bad2 ? "GAGAL" : "OK");
}

int main() { testFlat(); testAutoLevel(); testNoiseFreeze(); testSibilance(); testNaNAndSpeed(); }
