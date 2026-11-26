package com.example.dbxcomp

import android.content.Context
import android.media.VolumeProvider
import android.media.session.MediaSession
import android.media.session.PlaybackState
import android.os.Handler
import android.os.Looper

/**
 * "Volume luar": tombol volume fisik HP mengatur level akhir yang didengar saja (setelah titik
 * rekam), bukan volume sistem. Caranya: aplikasi mendaftarkan sesi media dengan volume jarak jauh
 * (seperti aplikasi cast). Selama sesi ini aktif dan aplikasi sedang memutar audio, Android
 * menyerahkan tombol volume ke sini, jadi volume sistem tidak berubah dan sinyal yang
 * ditangkap dari aplikasi lain tetap stabil.
 */
object OuterVolume {
    private const val MAX = 30
    private const val STEP_DB = 2f

    private var session: MediaSession? = null
    private var provider: VolumeProvider? = null
    private val main = Handler(Looper.getMainLooper())
    private var index = MAX

    /** Dipanggil (di thread utama) setiap volume berubah; nilai dalam dB (0 = penuh, -90 = senyap). */
    var onChanged: ((Float) -> Unit)? = null

    fun indexToDb(i: Int): Float = if (i <= 0) -90f else -(MAX - i) * STEP_DB

    fun dbToIndex(db: Float): Int =
        if (db <= -89f) 0 else (MAX + db / STEP_DB).toInt().coerceIn(1, MAX)

    fun start(context: Context, currentDb: Float) {
        stop()
        index = dbToIndex(currentDb)
        val p = object : VolumeProvider(VolumeProvider.VOLUME_CONTROL_ABSOLUTE, MAX, index) {
            override fun onSetVolumeTo(volume: Int) = applyIndex(volume)
            override fun onAdjustVolume(direction: Int) {
                if (direction != 0) applyIndex(index + direction)
            }
        }
        try {
            val s = MediaSession(context.applicationContext, "DbxCompVolumeLuar")
            s.setPlaybackState(
                PlaybackState.Builder()
                    .setActions(0)
                    .setState(PlaybackState.STATE_PLAYING, 0, 1f)
                    .build()
            )
            s.setPlaybackToRemote(p)
            s.isActive = true
            session = s
            provider = p
        } catch (e: Exception) {
            android.util.Log.w("DbxComp", "Sesi volume luar gagal dibuat", e)
        }
    }

    private fun applyIndex(newIndex: Int) {
        index = newIndex.coerceIn(0, MAX)
        provider?.currentVolume = index
        val db = indexToDb(index)
        main.post { onChanged?.invoke(db) }
    }

    /** Tombol volume yang ditangkap langsung oleh layar aplikasi (+1 naik, -1 turun). */
    fun adjust(direction: Int) = applyIndex(index + direction)

    fun currentDb(): Float = indexToDb(index)

    /** Sinkronkan indikator volume sistem bila nilai diubah dari slider di layar. */
    fun sync(db: Float) {
        index = dbToIndex(db)
        provider?.currentVolume = index
    }

    fun stop() {
        try {
            session?.isActive = false
            session?.release()
        } catch (_: Exception) {
        }
        session = null
        provider = null
    }
}
