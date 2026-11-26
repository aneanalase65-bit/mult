package com.example.dbxcomp

/** Jembatan JNI ke mesin audio native (Oboe). */
object AudioEngine {
    // ID parameter, harus sama dengan enum ParamId di audio_engine.cpp
    const val P_THRESH = 0
    const val P_RATIO = 1
    const val P_ATTACK = 2
    const val P_RELEASE = 3
    const val P_KNEE = 4
    const val P_MAKEUP = 5
    const val P_CEIL = 6
    const val P_LIM_ON = 7
    const val P_BYPASS = 8
    const val P_IN_GAIN = 9
    const val P_OUT_GAIN = 10
    const val P_LINK = 11
    const val P_GATE_L = 12
    const val P_GATE_R = 13
    const val P_IN_GAIN_INT = 14
    const val P_MONITOR = 15
    const val P_MIC_DYN = 16
    const val P_HPF_FREQ = 17
    const val P_GATE_THRESH = 18
    const val P_GATE_REL = 19
    const val P_REV_ON = 20
    const val P_REV_MIX = 21
    const val P_REV_SIZE = 22
    const val P_REV_DAMP = 23
    const val P_REV_PRE = 24
    const val P_REV_WIDTH = 25
    const val P_REV_LOWCUT = 26
    const val P_MUTE = 27
    const val P_TONE_ON = 28
    const val P_BASS = 29
    const val P_MID = 30
    const val P_TREBLE = 31
    const val P_STRIP = 32  // jumlah parameter per strip (A, B, C)

    // Parameter master (global, bukan per strip).
    const val P_M_GAIN = 32
    const val P_M_MUTE = 33
    const val P_M_LIM_ON = 34
    const val P_M_CEIL = 35
    const val P_M_OUTVOL = 36  // "volume luar" (tombol volume HP): setelah titik rekam
    // 3 EQ master x 6 parameter [on, 5 gain dB]; urutan: pre-master, pre-limiter, keluaran.
    const val P_EQ_BASE = 37
    const val EQ_COUNT = 3
    const val EQ_PARAMS = 6
    // EQ vokal (khusus strip A mic, global): [on, 10 gain dB]; pita 20 Hz - 20 kHz.
    const val P_VEQ_BASE = P_EQ_BASE + EQ_COUNT * EQ_PARAMS
    const val VEQ_BANDS = 10
    const val VEQ_PARAMS = VEQ_BANDS + 1
    // Compressor multiband + auto untuk vokal (khusus strip A mic, global); urutan = enum AcId di C++.
    const val P_AC_BASE = P_VEQ_BASE + VEQ_PARAMS
    const val AC_MODE = 0       // 0 = dBFS biasa, 1 = multiband
    const val AC_AUTO = 1       // 1 = threshold dan makeup otomatis
    const val AC_TARGET = 2     // dBFS RMS target
    const val AC_AMOUNT = 3     // % kekuatan kompresi
    const val AC_SIBILANCE = 4  // % kontrol sibilance
    const val AC_LOW_HZ = 5
    const val AC_HIGH_HZ = 6
    const val AC_NOISE = 7      // dBFS noise floor
    const val AC_PARAMS = 8
    const val P_COUNT = P_AC_BASE + AC_PARAMS

    /** ID parameter multiband/auto: [i] = AC_*. */
    fun acId(i: Int) = P_AC_BASE + i

    /** ID parameter EQ vokal: [i] 0 = on/off, 1..10 = gain tiap pita. */
    fun veqId(i: Int) = P_VEQ_BASE + i

    /** Benar selama mesin audio dijalankan; bertahan walau Activity dibuat ulang sistem. */
    @Volatile
    var isRunning = false

    /** Sumber (mask) yang berjalan saat [isRunning]. */
    @Volatile
    var activeMask = 0

    /** ID parameter EQ [k] (0..2); [i] 0 = on/off, 1..5 = gain tiap pita. */
    fun eqId(k: Int, i: Int) = P_EQ_BASE + k * EQ_PARAMS + i

    const val STRIPS = 3  // 0 = A mic, 1 = B internal HP, 2 = C internal ROOT
    const val SRC_MIC = 1
    const val SRC_INT = 2
    const val SRC_ROOT = 4

    /** ID yang tidak per strip: bypass, monitor, dan semua parameter master. */
    fun isGlobal(id: Int) = id == P_BYPASS || id == P_MONITOR || id >= P_STRIP

    /** Indeks parameter di mesin native; harus sama dengan enum ParamId/GlobalId di C++. */
    fun nativeIndex(strip: Int, id: Int): Int {
        val g = STRIPS * P_STRIP
        return when (id) {
            P_BYPASS -> g
            P_MONITOR -> g + 1
            P_M_GAIN -> g + 2
            P_M_MUTE -> g + 3
            P_M_LIM_ON -> g + 4
            P_M_CEIL -> g + 5
            P_M_OUTVOL -> g + 6
            else -> if (id >= P_EQ_BASE) g + 7 + (id - P_EQ_BASE) else strip * P_STRIP + id
        }
    }

    fun setParam(strip: Int, id: Int, value: Float) = nativeSetParam(nativeIndex(strip, id), value)

    init {
        System.loadLibrary("dbxcomp")
    }

    /**
     * [stereo] = minta input mic 2 channel; kalau perangkat menolak, otomatis turun ke mono.
     * [sourceMask] = kombinasi SRC_MIC, SRC_INT, SRC_ROOT (boleh lebih dari satu sekaligus);
     * [rate] = laju sampel tangkap internal (output Oboe disamakan).
     */
    external fun nativeStart(
        inputDeviceId: Int, outputDeviceId: Int, stereo: Boolean, sourceMask: Int, rate: Int
    ): Boolean

    /**
     * Dari thread tangkap: [source] 1 = B (MediaProjection), 2 = C (root);
     * [count] float stereo interleaved (L, R, L, R, ...).
     */
    external fun nativePushCapture(source: Int, data: FloatArray, count: Int)

    /** Mode rekam A/B (hanya sumber mono). Panggil sebelum rekaman dimulai. */
    external fun nativeSetRecordAB(ab: Boolean)

    /** Penguatan linear tambahan untuk strip B dan C (kompensasi volume sistem); 1.0 = mati. */
    external fun nativeSetCaptureComp(gain: Float)
    external fun nativeStop()
    external fun nativeSetParam(index: Int, value: Float)

    /**
     * Mengisi [out] (minimal 11 float): untuk tiap strip A, B, C: input peak dB, output peak dB,
     * gain reduction dB (positif); lalu output master peak dB dan gain reduction master.
     */
    external fun nativeGetMeters(out: FloatArray)

    /** Aktif/nonaktifkan penangkapan audio (pasangan asli+hasil) ke ring buffer. */
    external fun nativeSetRecording(on: Boolean)

    /** Mengambil data rekaman terkumpul ke [out]; mengembalikan jumlah float (genap). */
    external fun nativeReadRecorded(out: FloatArray): Int

    external fun nativeGetSampleRate(): Int

    /** Jumlah channel input yang benar-benar diberikan perangkat (1 atau 2). */
    external fun nativeGetInputChannels(): Int
}
