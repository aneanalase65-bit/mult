package com.example.dbxcomp

import android.content.Context
import android.os.Build
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Preset disimpan sebagai file JSON dengan NAMA parameter (bukan urutan angka), jadi tetap
 * terbaca walau nanti ada parameter baru. Isinya juga menyertakan versi aplikasi dan model HP.
 */
object PresetStore {
    const val SUB = "Preset"
    const val SUB_REKAMAN = "Rekaman"
    const val NETRAL = "Netral (bawaan)"
    const val NETRAL_FILE = "Netral.json"
    const val FORMAT = "dbxcomp-preset"

    /** Nama kunci untuk tiap ID parameter, urutannya sama dengan AudioEngine.P_*. */
    val KEYS = arrayOf(
        "threshold_db",        // 0  P_THRESH
        "ratio",               // 1  P_RATIO
        "attack_ms",           // 2  P_ATTACK
        "release_ms",          // 3  P_RELEASE
        "knee_db",             // 4  P_KNEE
        "makeup_db",           // 5  P_MAKEUP
        "ceiling_dbfs",        // 6  P_CEIL
        "limiter_on",          // 7  P_LIM_ON
        "bypass",              // 8  P_BYPASS
        "input_gain_mic_db",   // 9  P_IN_GAIN
        "output_gain_db",      // 10 P_OUT_GAIN
        "link_lr",             // 11 P_LINK
        "kanal_l_aktif",       // 12 P_GATE_L
        "kanal_r_aktif",       // 13 P_GATE_R
        "input_gain_internal_db", // 14 P_IN_GAIN_INT
        "monitor_output",      // 15 P_MONITOR
        "mic_dinamis_on",      // 16 P_MIC_DYN
        "highpass_hz",         // 17 P_HPF_FREQ
        "gate_threshold_dbfs", // 18 P_GATE_THRESH
        "gate_release_ms",     // 19 P_GATE_REL
        "reverb_on",           // 20 P_REV_ON
        "reverb_mix_pct",      // 21 P_REV_MIX
        "reverb_size_pct",     // 22 P_REV_SIZE
        "reverb_damping_pct",  // 23 P_REV_DAMP
        "reverb_predelay_ms",  // 24 P_REV_PRE
        "reverb_width_pct",    // 25 P_REV_WIDTH
        "reverb_lowcut_hz",    // 26 P_REV_LOWCUT
        "input_mute",          // 27 P_MUTE
        "tone_on",             // 28 P_TONE_ON
        "tone_bass_db",        // 29 P_BASS
        "tone_mid_db",         // 30 P_MID
        "tone_treble_db"       // 31 P_TREBLE
    )

    /**
     * Nilai awal semua ID (sama dengan bawaan mesin native). Strip B dan C mulai dengan
     * input gain -6 dB karena suara internal HP biasanya sudah mendekati 0 dBFS.
     */
    fun defaultsFor(strip: Int): FloatArray {
        val d = FloatArray(AudioEngine.P_COUNT) { 0f }
        d[AudioEngine.P_THRESH] = -20f
        d[AudioEngine.P_RATIO] = 4f
        d[AudioEngine.P_ATTACK] = 10f
        d[AudioEngine.P_RELEASE] = 200f
        d[AudioEngine.P_KNEE] = 6f
        d[AudioEngine.P_CEIL] = -1f
        d[AudioEngine.P_LIM_ON] = 1f
        d[AudioEngine.P_LINK] = 1f
        d[AudioEngine.P_GATE_L] = 1f
        d[AudioEngine.P_GATE_R] = 1f
        d[AudioEngine.P_MONITOR] = 1f
        d[AudioEngine.P_HPF_FREQ] = 100f
        d[AudioEngine.P_GATE_THRESH] = -55f
        d[AudioEngine.P_GATE_REL] = 200f
        d[AudioEngine.P_REV_MIX] = 20f
        d[AudioEngine.P_REV_SIZE] = 60f
        d[AudioEngine.P_REV_DAMP] = 50f
        d[AudioEngine.P_REV_PRE] = 15f
        d[AudioEngine.P_REV_WIDTH] = 100f
        d[AudioEngine.P_REV_LOWCUT] = 200f
        d[AudioEngine.P_IN_GAIN] = if (strip == 0) 0f else -6f
        d[AudioEngine.P_M_LIM_ON] = 1f
        d[AudioEngine.P_M_CEIL] = -1f
        d[AudioEngine.acId(AudioEngine.AC_MODE)] = 0f
        d[AudioEngine.acId(AudioEngine.AC_AUTO)] = 1f
        d[AudioEngine.acId(AudioEngine.AC_TARGET)] = -18f
        d[AudioEngine.acId(AudioEngine.AC_AMOUNT)] = 50f
        d[AudioEngine.acId(AudioEngine.AC_SIBILANCE)] = 50f
        d[AudioEngine.acId(AudioEngine.AC_LOW_HZ)] = 200f
        d[AudioEngine.acId(AudioEngine.AC_HIGH_HZ)] = 4500f
        d[AudioEngine.acId(AudioEngine.AC_NOISE)] = -50f
        return d
    }

    /**
     * Preset Netral: semua efek mati dan semua gain 0 dB. NaN = tidak disentuh
     * (kanal, monitor, bypass, dan link dibiarkan seperti keadaan sekarang).
     */
    val NEUTRAL: FloatArray = FloatArray(AudioEngine.P_COUNT) { Float.NaN }.also {
        it[AudioEngine.P_THRESH] = 0f
        it[AudioEngine.P_RATIO] = 1f
        it[AudioEngine.P_ATTACK] = 10f
        it[AudioEngine.P_RELEASE] = 200f
        it[AudioEngine.P_KNEE] = 0f
        it[AudioEngine.P_MAKEUP] = 0f
        it[AudioEngine.P_CEIL] = 0f
        it[AudioEngine.P_LIM_ON] = 0f
        it[AudioEngine.P_IN_GAIN] = 0f
        it[AudioEngine.P_OUT_GAIN] = 0f
        it[AudioEngine.P_IN_GAIN_INT] = 0f
        it[AudioEngine.P_MIC_DYN] = 0f
        it[AudioEngine.P_HPF_FREQ] = 100f
        it[AudioEngine.P_GATE_THRESH] = -55f
        it[AudioEngine.P_GATE_REL] = 200f
        it[AudioEngine.P_REV_ON] = 0f
        it[AudioEngine.P_REV_MIX] = 0f
        it[AudioEngine.P_REV_SIZE] = 60f
        it[AudioEngine.P_REV_DAMP] = 50f
        it[AudioEngine.P_REV_PRE] = 15f
        it[AudioEngine.P_REV_WIDTH] = 100f
        it[AudioEngine.P_REV_LOWCUT] = 200f
        it[AudioEngine.P_MUTE] = 0f
        it[AudioEngine.P_TONE_ON] = 0f
        it[AudioEngine.P_BASS] = 0f
        it[AudioEngine.P_MID] = 0f
        it[AudioEngine.P_TREBLE] = 0f
    }

    /** Kunci parameter master (global) di file preset. */
    val MASTER_KEYS = arrayOf(
        AudioEngine.P_M_GAIN to "master_gain_db",
        AudioEngine.P_M_MUTE to "master_mute",
        AudioEngine.P_M_LIM_ON to "master_limiter_on",
        AudioEngine.P_M_CEIL to "master_ceiling_dbfs",
        AudioEngine.P_M_OUTVOL to "volume_luar_db"
    )
    val EQ_NAMES = arrayOf("eq_pre_master", "eq_pre_limiter", "eq_keluaran")
    val EQ_KEYS = arrayOf("on", "hz80_db", "hz250_db", "hz1000_db", "hz4000_db", "hz10000_db")
    val VEQ_KEYS = arrayOf(
        "on", "hz40_shelf_db", "hz80_db", "hz160_db", "hz315_db", "hz630_db",
        "hz1250_db", "hz2500_db", "hz5000_db", "hz10000_db", "hz14000_shelf_db"
    )
    val AC_KEYS = arrayOf(
        "multiband_on", "auto_on", "target_rms_dbfs", "kekuatan_pct", "sibilance_pct",
        "crossover_rendah_hz", "crossover_tinggi_hz", "noise_floor_dbfs"
    )
    val STRIP_NAMES = arrayOf("A_mic", "B_internal", "C_root")

    /**
     * Isi preset. [scene] = true: preset global (formatVersion 2) berisi ketiga strip, master,
     * EQ, dan sumber aktif. false: preset lama (satu strip) yang nilainya ada di strips[0] dan
     * diterapkan ke strip yang sedang dipilih. [mask] = -1 bila tidak tercatat.
     */
    class Parsed(
        val name: String, val note: String,
        val strips: Array<FloatArray>, val globals: FloatArray,
        val mask: Int, val scene: Boolean
    )

    /** Preset Netral (global): semua efek mati, semua gain 0 dB, limiter master tetap aktif. */
    fun neutralScene(): Parsed {
        val strips = Array(AudioEngine.STRIPS) { NEUTRAL.copyOf() }
        val g = FloatArray(AudioEngine.P_COUNT) { Float.NaN }
        g[AudioEngine.P_M_GAIN] = 0f
        g[AudioEngine.P_M_MUTE] = 0f
        g[AudioEngine.P_M_LIM_ON] = 1f
        g[AudioEngine.P_M_CEIL] = -1f
        g[AudioEngine.P_M_OUTVOL] = 0f
        for (k in 0 until AudioEngine.EQ_COUNT) {
            for (i in 0 until AudioEngine.EQ_PARAMS) g[AudioEngine.eqId(k, i)] = 0f
        }
        for (i in 0 until AudioEngine.VEQ_PARAMS) g[AudioEngine.veqId(i)] = 0f
        g[AudioEngine.acId(AudioEngine.AC_MODE)] = 0f  // multiband mati; setelan lain dibiarkan
        return Parsed("Netral", "Semua efek mati, semua gain 0 dB (pembanding)", strips, g, -1, true)
    }

    fun fileNameFor(name: String): String {
        val sb = StringBuilder()
        for (ch in name.trim()) {
            val ok = ch.isLetterOrDigit() || ch == ' ' || ch == '-' || ch == '_' ||
                ch == '.' || ch == '(' || ch == ')'
            sb.append(if (ok) ch else '_')
        }
        var s = sb.toString().trim().trimEnd('.')
        if (s.isEmpty()) s = "Preset"
        return "$s.json"
    }

    fun nameFromFile(fileName: String): String = fileName.removeSuffix(".json")

    private fun round4(v: Float): Double = String.format(Locale.US, "%.4f", v).toDouble()

    fun paramsJson(values: FloatArray): JSONObject {
        val p = JSONObject()
        for (i in KEYS.indices) {
            if (i < values.size && !values[i].isNaN()) p.put(KEYS[i], round4(values[i]))
        }
        return p
    }

    @Suppress("DEPRECATION")
    fun appVersion(context: Context): String {
        return try {
            context.packageManager.getPackageInfo(context.packageName, 0).versionName ?: "?"
        } catch (e: Exception) {
            "?"
        }
    }

    fun deviceJson(): JSONObject {
        val d = JSONObject()
        d.put("merek", Build.MANUFACTURER)
        d.put("model", Build.MODEL)
        d.put("android_sdk", Build.VERSION.SDK_INT)
        return d
    }

    fun nowIso(): String =
        SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ssZ", Locale.US).format(Date())

    /** Preset global: ketiga strip, master, ketiga EQ, dan sumber aktif. */
    fun toJson(
        context: Context, name: String, note: String,
        strips: Array<FloatArray>, globals: FloatArray, mask: Int
    ): JSONObject {
        val o = JSONObject()
        o.put("format", FORMAT)
        o.put("formatVersion", 2)
        o.put("name", name)
        o.put("note", note)
        o.put("createdAt", nowIso())
        val app = JSONObject()
        app.put("nama", "DbxComp")
        app.put("versi", appVersion(context))
        o.put("app", app)
        o.put("device", deviceJson())

        val sj = JSONObject()
        for (st in 0 until AudioEngine.STRIPS) sj.put(STRIP_NAMES[st], paramsJson(strips[st]))
        o.put("strips", sj)

        val m = JSONObject()
        for ((id, key) in MASTER_KEYS) if (!globals[id].isNaN()) m.put(key, round4(globals[id]))
        o.put("master", m)

        val e = JSONObject()
        for (k in 0 until AudioEngine.EQ_COUNT) {
            val q = JSONObject()
            for (i in 0 until AudioEngine.EQ_PARAMS) {
                val v = globals[AudioEngine.eqId(k, i)]
                if (!v.isNaN()) q.put(EQ_KEYS[i], round4(v))
            }
            e.put(EQ_NAMES[k], q)
        }
        o.put("eq", e)

        val ve = JSONObject()
        for (i in 0 until AudioEngine.VEQ_PARAMS) {
            val v = globals[AudioEngine.veqId(i)]
            if (!v.isNaN()) ve.put(VEQ_KEYS[i], round4(v))
        }
        o.put("eq_vokal", ve)

        val ac = JSONObject()
        for (i in 0 until AudioEngine.AC_PARAMS) {
            val v = globals[AudioEngine.acId(i)]
            if (!v.isNaN()) ac.put(AC_KEYS[i], round4(v))
        }
        o.put("compressor_vokal", ac)

        if (mask >= 0) {
            val a = org.json.JSONArray()
            if (mask and AudioEngine.SRC_MIC != 0) a.put("A")
            if (mask and AudioEngine.SRC_INT != 0) a.put("B")
            if (mask and AudioEngine.SRC_ROOT != 0) a.put("C")
            o.put("sumber_aktif", a)
        }
        return o
    }

    /** Format lama (satu strip), dipakai untuk memigrasi preset dari penyimpanan lama. */
    fun toJsonSingle(context: Context, name: String, note: String, values: FloatArray): JSONObject {
        val o = JSONObject()
        o.put("format", FORMAT)
        o.put("formatVersion", 1)
        o.put("name", name)
        o.put("note", note)
        o.put("createdAt", nowIso())
        val app = JSONObject()
        app.put("nama", "DbxComp")
        app.put("versi", appVersion(context))
        o.put("app", app)
        o.put("device", deviceJson())
        o.put("params", paramsJson(values))
        return o
    }

    private fun readParams(p: JSONObject, out: FloatArray) {
        for (i in KEYS.indices) {
            if (i < out.size && p.has(KEYS[i])) out[i] = p.getDouble(KEYS[i]).toFloat()
        }
    }

    fun parse(text: String): Parsed? {
        try {
            val o = JSONObject(text)
            if (o.optString("format") != FORMAT) return null
            val strips = Array(AudioEngine.STRIPS) { FloatArray(AudioEngine.P_COUNT) { Float.NaN } }
            val globals = FloatArray(AudioEngine.P_COUNT) { Float.NaN }
            val sj = o.optJSONObject("strips")
            if (sj != null) {
                for (st in 0 until AudioEngine.STRIPS) {
                    val p = sj.optJSONObject(STRIP_NAMES[st]) ?: continue
                    readParams(p, strips[st])
                }
                o.optJSONObject("master")?.let { m ->
                    for ((id, key) in MASTER_KEYS) if (m.has(key)) globals[id] = m.getDouble(key).toFloat()
                }
                o.optJSONObject("eq")?.let { e ->
                    for (k in 0 until AudioEngine.EQ_COUNT) {
                        val q = e.optJSONObject(EQ_NAMES[k]) ?: continue
                        for (i in 0 until AudioEngine.EQ_PARAMS) {
                            if (q.has(EQ_KEYS[i])) globals[AudioEngine.eqId(k, i)] = q.getDouble(EQ_KEYS[i]).toFloat()
                        }
                    }
                }
                o.optJSONObject("eq_vokal")?.let { ve ->
                    for (i in 0 until AudioEngine.VEQ_PARAMS) {
                        if (ve.has(VEQ_KEYS[i])) globals[AudioEngine.veqId(i)] = ve.getDouble(VEQ_KEYS[i]).toFloat()
                    }
                }
                o.optJSONObject("compressor_vokal")?.let { ac ->
                    for (i in 0 until AudioEngine.AC_PARAMS) {
                        if (ac.has(AC_KEYS[i])) globals[AudioEngine.acId(i)] = ac.getDouble(AC_KEYS[i]).toFloat()
                    }
                }
                var mask = -1
                o.optJSONArray("sumber_aktif")?.let { a ->
                    var m = 0
                    for (i in 0 until a.length()) {
                        when (a.getString(i)) {
                            "A" -> m = m or AudioEngine.SRC_MIC
                            "B" -> m = m or AudioEngine.SRC_INT
                            "C" -> m = m or AudioEngine.SRC_ROOT
                        }
                    }
                    if (m != 0) mask = m
                }
                return Parsed(o.optString("name"), o.optString("note"), strips, globals, mask, true)
            }
            val p = o.optJSONObject("params") ?: return null
            readParams(p, strips[0])
            return Parsed(o.optString("name"), o.optString("note"), strips, globals, -1, false)
        } catch (e: Exception) {
            return null
        }
    }
}
