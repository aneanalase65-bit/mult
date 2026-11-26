// Mesin audio latensi rendah dengan TIGA strip yang benar-benar terpisah:
//   strip 0 = A mic/line-in/USB (stream input Oboe)
//   strip 1 = B suara internal HP (MediaProjection, lewat ring buffer)
//   strip 2 = C suara internal ROOT (REMOTE_SUBMIX, lewat ring buffer)
// Tiap strip: gain + mute -> [high-pass + gate] -> tone -> compressor -> reverb -> limiter ->
// fader keluaran. Semua strip aktif dijumlahkan ke MASTER (gain, mute, limiter lookahead).
// Memakai Oboe (AAudio di Android 8.1+, OpenSL ES di bawahnya).

#include <android/log.h>
#include <jni.h>
#include <oboe/Oboe.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "compressor.h"
#include "effects.h"
#include "frame_ring.h"
#include "ring_buffer.h"

#define LOG_TAG "DbxComp"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

constexpr int NSTRIPS = 3;

// Parameter per strip. Urutan HARUS sama dengan AudioEngine.P_* di Kotlin.
enum ParamId {
    P_THRESH = 0,
    P_RATIO,
    P_ATTACK,
    P_RELEASE,
    P_KNEE,
    P_MAKEUP,
    P_CEIL,
    P_LIM_ON,
    P_BYPASS,       // (tidak dipakai per strip; bypass bersifat global: G_BYPASS)
    P_IN_GAIN,
    P_OUT_GAIN,
    P_LINK,
    P_GATE_L,
    P_GATE_R,
    P_IN_GAIN_INT,  // (tidak dipakai lagi; tiap strip punya P_IN_GAIN sendiri)
    P_MONITOR,      // (tidak dipakai per strip; monitor bersifat global: G_MONITOR)
    P_MIC_DYN,      // high-pass + noise gate
    P_HPF_FREQ,     // Hz
    P_GATE_THRESH,  // dBFS
    P_GATE_REL,     // ms
    P_REV_ON,
    P_REV_MIX,      // persen
    P_REV_SIZE,     // persen
    P_REV_DAMP,     // persen
    P_REV_PRE,      // ms
    P_REV_WIDTH,    // persen
    P_REV_LOWCUT,   // Hz
    P_MUTE,         // 1 = input strip dibisukan
    P_TONE_ON,
    P_BASS,         // dB
    P_MID,          // dB
    P_TREBLE,       // dB
    P_STRIP         // jumlah parameter per strip
};

// Parameter global (master), diletakkan setelah semua blok strip.
enum GlobalId {
    G_BYPASS = NSTRIPS * P_STRIP,
    G_MONITOR,
    G_M_GAIN,    // dB
    G_M_MUTE,
    G_M_LIM_ON,
    G_M_CEIL,    // dBFS
    G_M_OUTVOL,  // dB: "volume luar" (tombol volume HP), setelah titik rekam
    G_EQ_BASE,   // 3 EQ x 6 parameter: [on, 5 gain dB]; urutan EQ: pre-master, pre-limiter, keluaran
    G_VEQ_BASE = G_EQ_BASE + 3 * 6,  // EQ vokal strip A: [on, 10 gain dB]
    // Compressor multiband + auto untuk vokal (strip A saja). Ditaruh paling akhir supaya
    // indeks parameter lama tidak bergeser. Urutan HARUS sama dengan AudioEngine.AC_* di Kotlin.
    G_AC_BASE = G_VEQ_BASE + 11,
    P_TOTAL = G_AC_BASE + 8
};

enum AcId {
    AC_MODE = 0,   // 0 = dBFS biasa (lama), 1 = multiband
    AC_AUTO,       // 1 = threshold dan makeup otomatis
    AC_TARGET,     // dBFS RMS target
    AC_AMOUNT,     // % kekuatan kompresi
    AC_SIBILANCE,  // % kontrol sibilance
    AC_LOW_HZ,
    AC_HIGH_HZ,
    AC_NOISE       // dBFS noise floor
};

enum { SRC_MIC = 1, SRC_INT = 2, SRC_ROOT = 4 };

struct Strip {
    StereoChain chain;
    Reverb rev;
    HighPass hpL, hpR;
    Gate gate;
    Tone tone;
    VocalEq veq;  // dipakai strip A saja
    float muteG = 1.f;
    std::vector<float> src;  // input strip untuk blok ini (interleaved, srcCh channel)
    std::vector<float> out;  // keluaran strip untuk blok ini (stereo interleaved)
    std::vector<float> raw;  // L mentah setelah gain (untuk rekam A/B)
    int srcCh = 2;
    std::atomic<float> mIn{-100.f}, mOut{-100.f}, mGr{0.f};
    Strip() { chain.rev = &rev; }
};

class Engine : public oboe::AudioStreamDataCallback,
               public oboe::AudioStreamErrorCallback,
               public std::enable_shared_from_this<Engine> {
public:
    Engine() {
        static const float defaults[P_STRIP] = {
            -20.f, 4.f,   10.f,  200.f, 6.f,   0.f,   // thresh .. makeup
            -1.f,  1.f,   0.f,   0.f,   0.f,   1.f,   // ceil, limOn, bypass, inGain, outGain, link
            1.f,   1.f,   0.f,   1.f,                   // gateL, gateR, inGainInt, monitor
            0.f,   100.f, -55.f, 200.f,                 // micDyn, hpf, gateThr, gateRel
            0.f,   20.f,  60.f,  50.f,  15.f,  100.f, 200.f,  // reverb
            0.f,   0.f,   0.f,   0.f,   0.f};           // mute, toneOn, bass, mid, treble
        for (int s = 0; s < NSTRIPS; ++s) {
            for (int i = 0; i < P_STRIP; ++i) params[s * P_STRIP + i].store(defaults[i]);
        }
        // Suara internal biasanya sudah mendekati 0 dBFS: mulai dengan headroom 6 dB.
        params[1 * P_STRIP + P_IN_GAIN].store(-6.f);
        params[2 * P_STRIP + P_IN_GAIN].store(-6.f);
        params[G_BYPASS].store(0.f);
        params[G_MONITOR].store(1.f);
        params[G_M_GAIN].store(0.f);
        params[G_M_MUTE].store(0.f);
        params[G_M_LIM_ON].store(1.f);
        params[G_M_CEIL].store(-1.f);
        params[G_M_OUTVOL].store(0.f);
        for (int i = 0; i < 3 * 6; ++i) params[G_EQ_BASE + i].store(0.f);
        for (int i = 0; i < 11; ++i) params[G_VEQ_BASE + i].store(0.f);
        params[G_AC_BASE + AC_MODE].store(0.f);
        params[G_AC_BASE + AC_AUTO].store(1.f);
        params[G_AC_BASE + AC_TARGET].store(-18.f);
        params[G_AC_BASE + AC_AMOUNT].store(50.f);
        params[G_AC_BASE + AC_SIBILANCE].store(50.f);
        params[G_AC_BASE + AC_LOW_HZ].store(200.f);
        params[G_AC_BASE + AC_HIGH_HZ].store(4500.f);
        params[G_AC_BASE + AC_NOISE].store(-50.f);
    }

    void setParam(int idx, float v) {
        if (idx >= 0 && idx < P_TOTAL) params[idx].store(v, std::memory_order_relaxed);
    }

    // Mode rekam A/B (kiri = asli, kanan = hasil); hanya berlaku bila hanya strip A (mono) aktif.
    void setRecordAB(bool ab) { recAB.store(ab, std::memory_order_relaxed); }

    // Penguatan pengganti untuk strip B dan C (dihitung di Kotlin dari volume sistem).
    void setCaptureComp(float g) { capComp.store(g, std::memory_order_relaxed); }

    // Dari thread tangkap audio internal: source 1 = B (MediaProjection), 2 = C (root).
    void pushCapture(int source, const float* data, size_t frames) {
        if (source == 1) capRing[0].write(data, frames);
        else if (source == 2) capRing[1].write(data, frames);
    }

    void setRecording(bool on) {
        if (on) ring.clear();  // thread audio belum menulis karena flag masih false
        recording.store(on, std::memory_order_relaxed);
    }

    int readRecorded(float* out, int maxFloats) {
        return static_cast<int>(ring.read(out, static_cast<size_t>(std::max(0, maxFloats))));
    }

    int getSampleRate() const { return static_cast<int>(sampleRate); }

    // Jumlah channel input mic yang benar-benar diberikan perangkat (1 atau 2).
    int getInputChannels() const { return inChannels.load(std::memory_order_relaxed); }

    // out11: [strip0 in, out, gr][strip1 ...][strip2 ...] lalu master out dan master GR.
    void getMeters(float* out11) {
        for (int s = 0; s < NSTRIPS; ++s) {
            out11[s * 3 + 0] = strips[s].mIn.exchange(-100.f);
            out11[s * 3 + 1] = strips[s].mOut.exchange(-100.f);
            out11[s * 3 + 2] = strips[s].mGr.exchange(0.f);
        }
        out11[9] = meterOut.exchange(-100.f);
        out11[10] = meterGr.exchange(0.f);
    }

    bool start(int inId, int outId, bool stereo, int mask, int rate) {
        std::lock_guard<std::mutex> lock(mtx);
        if ((mask & (SRC_MIC | SRC_INT | SRC_ROOT)) == 0) return false;
        inputDeviceId = inId;
        outputDeviceId = outId;
        wantStereo = stereo;
        srcMask.store(mask);
        captureRate = rate;
        running = true;
        if (!openStreams()) {
            running = false;
            return false;
        }
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mtx);
        running = false;
        closeStreams();
    }

    float prm(int s, int id) const {
        return params[s * P_STRIP + id].load(std::memory_order_relaxed);
    }

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream, void* audioData,
                                          int32_t numFrames) override {
        float* out = static_cast<float*>(audioData);
        const int outCh = stream->getChannelCount();
        const int mask = srcMask.load(std::memory_order_relaxed);
        const int32_t nf = std::min<int32_t>(numFrames, static_cast<int32_t>(blockCap));
        if (numFrames > nf) std::fill(out + nf * outCh, out + numFrames * outCh, 0.f);

        const bool bypass = params[G_BYPASS].load(std::memory_order_relaxed) > 0.5f;
        const float mon = params[G_MONITOR].load(std::memory_order_relaxed) > 0.5f ? 1.f : 0.f;
        const bool rec = recording.load(std::memory_order_relaxed);
        const bool micOnly = (mask == SRC_MIC) && strips[0].srcCh < 2;
        const bool ab = rec && recAB.load(std::memory_order_relaxed) && micOnly;

        float inPeakDummy = 0.f;
        (void)inPeakDummy;

        // ---- 1) Proses tiap strip aktif ke buffer keluaran strip ----
        for (int s = 0; s < NSTRIPS; ++s) {
            if (!(mask & (1 << s))) continue;
            Strip& st = strips[s];
            int32_t got = 0;
            if (s == 0) {
                oboe::AudioStream* in = inStream.get();
                if (in != nullptr && nf > 0) {
                    const int32_t frames = std::min<int32_t>(
                        nf, static_cast<int32_t>(st.src.size() / static_cast<size_t>(st.srcCh)));
                    auto res = in->read(st.src.data(), frames, 0);
                    if (res.error() == oboe::Result::OK) {
                        got = res.value();
                    } else if (res.error() == oboe::Result::ErrorDisconnected ||
                               res.error() == oboe::Result::ErrorClosed) {
                        requestReopen();  // input mati (mis. direbut aplikasi lain): buka ulang
                    }
                }
            } else {
                // Audio internal datang dari thread tangkap lewat ring buffer. Tunggu terisi
                // ~30 ms dulu supaya tidak putus-putus; kalau menumpuk >120 ms, kejar.
                FrameRing& cr = capRing[s - 1];
                size_t avail = cr.available();
                const size_t maxF = static_cast<size_t>(sampleRate * 0.12f);
                const size_t tgtF = static_cast<size_t>(sampleRate * 0.03f);
                if (avail > maxF) {
                    cr.skip(avail - tgtF);
                    avail = tgtF;
                }
                if (!primed[s - 1] && avail >= tgtF) primed[s - 1] = true;
                if (primed[s - 1] && nf > 0) {
                    if (avail < static_cast<size_t>(nf)) primed[s - 1] = false;
                    got = static_cast<int32_t>(cr.read(st.src.data(), static_cast<size_t>(nf)));
                }
            }
            processStrip(s, nf, got, bypass, ab);
        }

        // ---- 2) Master: jumlahkan strip aktif, gain, mute, limiter lookahead ----
        const float mGain = dbToLinSilent(params[G_M_GAIN].load(std::memory_order_relaxed));
        const float mMuteTarget =
            params[G_M_MUTE].load(std::memory_order_relaxed) > 0.5f ? 0.f : 1.f;
        const bool mLimOn = params[G_M_LIM_ON].load(std::memory_order_relaxed) > 0.5f;
        mlim.prepare(sampleRate, params[G_M_CEIL].load(std::memory_order_relaxed));

        // EQ master: 0 = pre-master, 1 = pre-limiter, 2 = keluaran (setelah titik rekam).
        bool eqOn[3];
        for (int k = 0; k < 3; ++k) {
            const int base = G_EQ_BASE + k * 6;
            eqOn[k] = params[base].load(std::memory_order_relaxed) > 0.5f;
            float gdb[Eq5::kBands];
            for (int b = 0; b < Eq5::kBands; ++b) {
                gdb[b] = params[base + 1 + b].load(std::memory_order_relaxed);
            }
            eq[k].setup(static_cast<float>(sampleRate), gdb);
        }
        const float outVolTarget = dbToLinSilent(params[G_M_OUTVOL].load(std::memory_order_relaxed));

        float outPeak = 0.f, minGr = 0.f;
        for (int32_t i = 0; i < nf; ++i) {
            float l = 0.f, r = 0.f;
            for (int s = 0; s < NSTRIPS; ++s) {
                if (!(mask & (1 << s))) continue;
                l += strips[s].out[static_cast<size_t>(i) * 2];
                r += strips[s].out[static_cast<size_t>(i) * 2 + 1];
            }
            if (eqOn[0]) eq[0].process(l, r);  // EQ pre-master
            mMuteG += (mMuteTarget - mMuteG) * kMuteCoef;
            if (mMuteTarget == 0.f && mMuteG < 1e-4f) mMuteG = 0.f;
            const float g = mGain * mMuteG;
            l *= g;
            r *= g;
            if (eqOn[1]) eq[1].process(l, r);  // EQ pre-limiter
            mlim.process(l, r, mLimOn);
            l = std::max(-1.f, std::min(1.f, l));  // jangan pernah clip digital di output
            r = std::max(-1.f, std::min(1.f, r));

            // Rekaman: normal -> (kiri, kanan) master; A/B (mono) -> (asli, hasil).
            if (rec) {
                if (ab) ring.push2(strips[0].raw[static_cast<size_t>(i)], l);
                else ring.push2(l, r);
            }
            // Meter output dihitung di titik rekam: tidak terpengaruh EQ keluaran dan volume luar.
            outPeak = std::max(outPeak, std::max(std::fabs(l), std::fabs(r)));
            minGr = std::min(minGr, mlim.gr);

            // ---- Hanya untuk telinga (tidak masuk rekaman): EQ keluaran + volume luar ----
            float ol = l, orr = r;
            if (eqOn[2]) eq[2].process(ol, orr);
            outVolG += (outVolTarget - outVolG) * kOutVolCoef;
            ol = softClip(ol * outVolG);
            orr = softClip(orr * outVolG);
            for (int c = 0; c < outCh; ++c) out[i * outCh + c] = ((c == 0) ? ol : orr) * mon;
        }
        publishMax(meterOut, 20.f * std::log10(outPeak + 1e-6f));
        publishMax(meterGr, -minGr);
        return oboe::DataCallbackResult::Continue;
    }

    // Dipanggil Oboe di thread terpisah kalau perangkat audio terputus (mis. USB dicabut).
    // Semua kesalahan stream (bukan hanya perangkat terputus) mencoba membuka ulang stream,
    // supaya audio tidak mati sendiri saat aplikasi di latar belakang.
    void onErrorAfterClose(oboe::AudioStream*, oboe::Result) override {
        if (reopening.exchange(true)) return;
        reopenNow();
        reopening.store(false);
    }

    // Dari thread audio: jangan membuka stream di sini, serahkan ke thread terpisah.
    void requestReopen() {
        if (reopening.exchange(true)) return;
        auto self = shared_from_this();
        std::thread([self] {
            self->reopenNow();
            self->reopening.store(false);
        }).detach();
    }

    void reopenNow() {
        for (int attempt = 0; attempt < 5; ++attempt) {
            {
                std::lock_guard<std::mutex> lock(mtx);
                if (!running) return;
                if (openStreams()) return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }

private:
    static constexpr float kMuteCoef = 0.01f;  // ~2 ms (tanpa bunyi "klik")
    static constexpr float kOutVolCoef = 0.004f;  // ~5 ms: volume luar tanpa "zipper noise"

    // Pengaman terakhir di output: lurus sampai 0.9, di atasnya melengkung halus menuju 1.0.
    static float softClip(float x) {
        const float a = std::fabs(x);
        if (a <= 0.9f) return x;
        const float y = 0.9f + 0.1f * std::tanh((a - 0.9f) * 10.f);
        return x < 0.f ? -y : y;
    }

    static void publishMax(std::atomic<float>& a, float v) {
        if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed);
    }

    // Satu strip, satu blok. Hasil ditulis ke strips[s].out (stereo interleaved).
    void processStrip(int s, int32_t nf, int32_t got, bool bypass, bool ab) {
        Strip& st = strips[s];
        const int inCh = st.srcCh;
        const bool srcStereo = inCh >= 2;

        ProcParams pp;
        pp.comp.thresholdDb = prm(s, P_THRESH);
        pp.comp.ratio = prm(s, P_RATIO);
        pp.comp.attackMs = prm(s, P_ATTACK);
        pp.comp.releaseMs = prm(s, P_RELEASE);
        pp.comp.kneeDb = prm(s, P_KNEE);
        pp.comp.makeupDb = prm(s, P_MAKEUP);
        pp.ceilDb = prm(s, P_CEIL);
        pp.limOn = prm(s, P_LIM_ON) > 0.5f;
        pp.link = prm(s, P_LINK) > 0.5f;
        pp.outGain = dbToLinSilent(prm(s, P_OUT_GAIN));
        // Strip B/C: kompensasi penurunan level akibat volume sistem (1.0 = mati).
        const float inG = dbToLinSilent(prm(s, P_IN_GAIN)) *
                          (s > 0 ? capComp.load(std::memory_order_relaxed) : 1.f);
        const float muteTarget = prm(s, P_MUTE) > 0.5f ? 0.f : 1.f;

        const bool revOn = prm(s, P_REV_ON) > 0.5f;
        pp.revOn = revOn;
        if (s == 0) {  // multiband + auto: khusus strip A (mic vokal, 1 channel)
            auto ac = [this](int i) { return params[G_AC_BASE + i].load(std::memory_order_relaxed); };
            pp.mb.on = ac(AC_MODE) > 0.5f;
            pp.mb.autoOn = ac(AC_AUTO) > 0.5f;
            pp.mb.targetDb = ac(AC_TARGET);
            pp.mb.amount = ac(AC_AMOUNT);
            pp.mb.sibilance = ac(AC_SIBILANCE);
            pp.mb.lowHz = ac(AC_LOW_HZ);
            pp.mb.highHz = ac(AC_HIGH_HZ);
            pp.mb.noiseFloorDb = ac(AC_NOISE);
        }
        st.rev.setParams(prm(s, P_REV_SIZE), prm(s, P_REV_DAMP), prm(s, P_REV_PRE),
                         prm(s, P_REV_WIDTH), prm(s, P_REV_LOWCUT), prm(s, P_REV_MIX));

        const bool dynOn = prm(s, P_MIC_DYN) > 0.5f;
        st.hpL.setup(sampleRate, prm(s, P_HPF_FREQ));
        st.hpR.setup(sampleRate, prm(s, P_HPF_FREQ));
        st.gate.prepare(sampleRate, prm(s, P_GATE_REL));
        const float gateThr = dbToLin(prm(s, P_GATE_THRESH));

        const bool toneOn = prm(s, P_TONE_ON) > 0.5f;
        st.tone.setup(sampleRate, prm(s, P_BASS), prm(s, P_MID), prm(s, P_TREBLE));

        st.chain.prepare(sampleRate, pp);

        // EQ vokal (hanya strip A): dipasang di akhir strip, sebelum dijumlahkan ke master,
        // jadi ikut terekam dan ikut ke keluaran global.
        const bool veqOn = (s == 0) && !bypass &&
                           params[G_VEQ_BASE].load(std::memory_order_relaxed) > 0.5f;
        if (veqOn) {
            float gv[VocalEq::kBands];
            for (int b = 0; b < VocalEq::kBands; ++b) {
                gv[b] = params[G_VEQ_BASE + 1 + b].load(std::memory_order_relaxed);
            }
            st.veq.setup(sampleRate, gv);
        }

        // Rekam A/B (mono): gate L/R diabaikan.
        const bool useAB = ab && s == 0;
        const bool gateL = useAB || prm(s, P_GATE_L) > 0.5f;
        const bool gateR = useAB || prm(s, P_GATE_R) > 0.5f;
        // Jalur stereo dipakai kalau sumber stereo, atau mono tapi satu sisi dimatikan.
        // Reverb selalu butuh jalur stereo (ekornya kiri-kanan berbeda).
        const bool stereo = srcStereo || !(gateL && gateR) || revOn;

        float inPeak = 0.f, outPeak = 0.f, maxReduction = 0.f;
        for (int32_t i = 0; i < nf; ++i) {
            const bool have = i < got;
            const float rawL = have ? st.src[static_cast<size_t>(i) * inCh] : 0.f;
            const float rawR = (have && srcStereo) ? st.src[static_cast<size_t>(i) * inCh + 1] : rawL;

            st.muteG += (muteTarget - st.muteG) * kMuteCoef;
            if (muteTarget == 0.f && st.muteG < 1e-4f) st.muteG = 0.f;
            const float g = inG * st.muteG;

            float l = gateL ? rawL * g : 0.f;
            float r = gateR ? rawR * g : 0.f;
            if (s == 0) st.raw[static_cast<size_t>(i)] = l;

            if (!bypass) {
                if (dynOn) {
                    // High-pass lalu noise gate (terhubung L/R) sebelum compressor.
                    l = st.hpL.process(l);
                    if (stereo) r = st.hpR.process(r);
                    const float lvl = stereo ? std::max(std::fabs(l), std::fabs(r)) : std::fabs(l);
                    const float gg = st.gate.process(lvl, gateThr);
                    l *= gg;
                    r *= gg;
                }
                if (toneOn) {
                    l = st.tone.processL(l);
                    if (stereo) r = st.tone.processR(r);
                }
            }

            float yl, yr;
            if (bypass) {
                yl = l;
                yr = stereo ? r : l;
            } else {
                float red = 0.f;
                st.chain.process(l, r, stereo, pp, yl, yr, red);
                maxReduction = std::max(maxReduction, red);
            }
            if (veqOn) {
                if (stereo) {
                    st.veq.process(yl, yr);
                } else {
                    yl = st.veq.processMono(yl);
                    yr = yl;
                }
            }
            st.out[static_cast<size_t>(i) * 2] = yl;
            st.out[static_cast<size_t>(i) * 2 + 1] = yr;

            inPeak = std::max(inPeak, std::max(std::fabs(l), stereo ? std::fabs(r) : 0.f));
            outPeak = std::max(outPeak, std::max(std::fabs(yl), std::fabs(yr)));
        }
        publishMax(st.mIn, 20.f * std::log10(inPeak + 1e-6f));
        publishMax(st.mOut, 20.f * std::log10(outPeak + 1e-6f));
        publishMax(st.mGr, maxReduction);
    }

    bool openInput(int channels) {
        oboe::AudioStreamBuilder ib;
        ib.setDirection(oboe::Direction::Input)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Exclusive)
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(channels)
            ->setSampleRate(outStream->getSampleRate())
            ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
            // Stereo: preset Camcorder (jalur yang dipakai kamera untuk dua mic).
            // Mono: Unprocessed (matikan AGC/noise suppression).
            ->setInputPreset(channels == 2 ? oboe::InputPreset::Camcorder
                                           : oboe::InputPreset::Unprocessed)
            ->setDeviceId(inputDeviceId);
        const oboe::Result r = ib.openStream(inStream);
        if (r != oboe::Result::OK) {
            LOGE("Gagal membuka input %d channel: %s", channels, oboe::convertToText(r));
            inStream.reset();
            return false;
        }
        return true;
    }

    bool openStreams() {
        closeStreams();
        const int mask = srcMask.load();

        oboe::AudioStreamBuilder ob;
        ob.setDirection(oboe::Direction::Output)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Exclusive)
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(oboe::ChannelCount::Stereo)
            ->setUsage(oboe::Usage::Media)
            ->setDeviceId(outputDeviceId)
            ->setDataCallback(shared_from_this())
            ->setErrorCallback(shared_from_this());
        if ((mask & (SRC_INT | SRC_ROOT)) != 0 && captureRate > 0) {
            // Samakan laju sampel output dengan laju tangkap internal (resample kalau perlu).
            ob.setSampleRate(captureRate)
                ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium);
        }
        oboe::Result r = ob.openStream(outStream);
        if (r != oboe::Result::OK) {
            LOGE("Gagal membuka output: %s", oboe::convertToText(r));
            outStream.reset();
            return false;
        }
        outStream->setBufferSizeInFrames(outStream->getFramesPerBurst() * 2);
        sampleRate = static_cast<float>(outStream->getSampleRate());

        if ((mask & SRC_MIC) != 0) {
            // Coba stereo dulu kalau diminta; kalau perangkat menolak, turun ke mono.
            bool inOk = false;
            if (wantStereo) inOk = openInput(2);
            if (!inOk) inOk = openInput(1);
            if (!inOk) {
                outStream->close();
                outStream.reset();
                return false;
            }
            inChannels.store(std::max(1, inStream->getChannelCount()));
        } else {
            inChannels.store(2);
        }

        blockCap = static_cast<size_t>(std::max<int32_t>(8192, outStream->getFramesPerBurst() * 8));
        for (int s = 0; s < NSTRIPS; ++s) {
            Strip& st = strips[s];
            st.srcCh = (s == 0) ? std::max(1, inChannels.load()) : 2;
            st.src.assign(blockCap * static_cast<size_t>(st.srcCh), 0.f);
            st.out.assign(blockCap * 2, 0.f);
            st.raw.assign(blockCap, 0.f);
            st.rev.init(sampleRate);  // alokasi buffer reverb (stream sedang tertutup)
            st.hpL.reset();
            st.hpR.reset();
            st.gate.reset();
            st.tone.reset();
            st.veq.reset();
            st.chain.reset();
            st.muteG = 1.f;
        }
        for (int k = 0; k < 2; ++k) {
            capRing[k].clear();
            primed[k] = false;
        }
        mlim.reset();
        for (auto& e : eq) e.reset();
        mMuteG = 1.f;
        outVolG = dbToLinSilent(params[G_M_OUTVOL].load(std::memory_order_relaxed));

        if (inStream) inStream->requestStart();
        outStream->requestStart();
        return true;
    }

    void closeStreams() {
        // Output ditutup lebih dulu supaya callback berhenti sebelum input dilepas.
        if (outStream) {
            outStream->stop();
            outStream->close();
            outStream.reset();
        }
        if (inStream) {
            inStream->stop();
            inStream->close();
            inStream.reset();
        }
    }

    std::mutex mtx;
    std::atomic<bool> reopening{false};
    bool running = false;
    bool wantStereo = false;
    int captureRate = 48000;
    std::atomic<int> srcMask{SRC_MIC};
    bool primed[2] = {false, false};  // hanya diakses thread audio (dan saat stream tertutup)
    int inputDeviceId = 0;
    int outputDeviceId = 0;
    float sampleRate = 48000.f;
    size_t blockCap = 8192;
    std::atomic<int> inChannels{1};

    std::shared_ptr<oboe::AudioStream> outStream;
    std::shared_ptr<oboe::AudioStream> inStream;

    std::atomic<float> params[P_TOTAL];
    std::atomic<float> meterOut{-100.f};
    std::atomic<float> meterGr{0.f};

    RingBuffer ring;
    FrameRing capRing[2];
    std::atomic<bool> recording{false};
    std::atomic<bool> recAB{false};
    std::atomic<float> capComp{1.f};

    Strip strips[NSTRIPS];
    LookaheadLimiter mlim;
    Eq5 eq[3];
    float outVolG = 1.f;
    float mMuteG = 1.f;
};

static std::shared_ptr<Engine> gEngine;

static Engine& engine() {
    if (!gEngine) gEngine = std::make_shared<Engine>();
    return *gEngine;
}

extern "C" {

JNIEXPORT jboolean JNICALL Java_com_example_dbxcomp_AudioEngine_nativeStart(
    JNIEnv*, jobject, jint inputDeviceId, jint outputDeviceId, jboolean stereo,
    jint sourceMask, jint rate) {
    return engine().start(inputDeviceId, outputDeviceId, stereo == JNI_TRUE, sourceMask, rate)
               ? JNI_TRUE
               : JNI_FALSE;
}

// Dari thread tangkap Kotlin: source 1 = B (MediaProjection), 2 = C (root);
// [count] float stereo interleaved. Buffer terpisah per sumber (dua thread bisa bersamaan).
JNIEXPORT void JNICALL Java_com_example_dbxcomp_AudioEngine_nativePushCapture(
    JNIEnv* env, jobject, jint source, jfloatArray data, jint count) {
    static float tmp[2][16384];
    if (source < 1 || source > 2) return;
    float* buf = tmp[source - 1];
    const jint n = std::min<jint>(std::min<jint>(count, env->GetArrayLength(data)), 16384) & ~1;
    if (n <= 0) return;
    env->GetFloatArrayRegion(data, 0, n, buf);
    engine().pushCapture(source, buf, static_cast<size_t>(n / 2));
}

JNIEXPORT void JNICALL Java_com_example_dbxcomp_AudioEngine_nativeSetCaptureComp(
    JNIEnv*, jobject, jfloat gain) {
    engine().setCaptureComp(gain);
}

JNIEXPORT void JNICALL Java_com_example_dbxcomp_AudioEngine_nativeSetRecordAB(
    JNIEnv*, jobject, jboolean ab) {
    engine().setRecordAB(ab == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_example_dbxcomp_AudioEngine_nativeStop(JNIEnv*, jobject) {
    engine().stop();
}

JNIEXPORT void JNICALL Java_com_example_dbxcomp_AudioEngine_nativeSetParam(
    JNIEnv*, jobject, jint id, jfloat value) {
    engine().setParam(id, value);
}

JNIEXPORT void JNICALL Java_com_example_dbxcomp_AudioEngine_nativeGetMeters(
    JNIEnv* env, jobject, jfloatArray out) {
    jfloat tmp[11];
    engine().getMeters(tmp);
    const jsize n = std::min<jsize>(env->GetArrayLength(out), 11);
    env->SetFloatArrayRegion(out, 0, n, tmp);
}

JNIEXPORT void JNICALL Java_com_example_dbxcomp_AudioEngine_nativeSetRecording(
    JNIEnv*, jobject, jboolean on) {
    engine().setRecording(on == JNI_TRUE);
}

JNIEXPORT jint JNICALL Java_com_example_dbxcomp_AudioEngine_nativeReadRecorded(
    JNIEnv* env, jobject, jfloatArray out) {
    const jsize cap = env->GetArrayLength(out);
    jfloat* p = env->GetFloatArrayElements(out, nullptr);
    if (p == nullptr) return 0;
    const int n = engine().readRecorded(p, cap);
    env->ReleaseFloatArrayElements(out, p, 0);
    return n;
}

JNIEXPORT jint JNICALL Java_com_example_dbxcomp_AudioEngine_nativeGetSampleRate(
    JNIEnv*, jobject) {
    return engine().getSampleRate();
}

JNIEXPORT jint JNICALL Java_com_example_dbxcomp_AudioEngine_nativeGetInputChannels(
    JNIEnv*, jobject) {
    return engine().getInputChannels();
}

}  // extern "C"
