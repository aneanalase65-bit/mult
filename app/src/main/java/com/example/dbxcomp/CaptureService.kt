package com.example.dbxcomp

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Process
import android.util.Log
import kotlin.math.max

/**
 * Menangkap suara internal HP (AudioPlaybackCapture, stereo float) lalu mendorongnya ke
 * mesin native. Harus berupa layanan foreground bertipe mediaProjection (syarat Android 10+).
 */
class CaptureService : Service() {

    companion object {
        const val EXTRA_CODE = "code"
        const val EXTRA_DATA = "data"
        const val EXTRA_RATE = "rate"
        private const val CHANNEL_ID = "dbxcomp_capture"
        private const val NOTIF_ID = 1

        /** Dipanggil di thread utama kalau tangkap gagal/berhenti tak terduga. */
        @Volatile
        var listener: ((String) -> Unit)? = null
    }

    private val main = Handler(Looper.getMainLooper())
    private var projection: MediaProjection? = null
    @Volatile
    private var record: AudioRecord? = null
    private var thread: Thread? = null

    @Volatile
    private var capturing = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent == null) {
            stopSelf()
            return START_NOT_STICKY
        }
        startForegroundCompat()
        val code = intent.getIntExtra(EXTRA_CODE, 0)
        val rate = intent.getIntExtra(EXTRA_RATE, 48000)
        val data: Intent? =
            if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(EXTRA_DATA, Intent::class.java)
            else @Suppress("DEPRECATION") intent.getParcelableExtra(EXTRA_DATA)
        val err = begin(code, data, rate)
        if (err != null) {
            Log.e("DbxComp", err)
            report(err)
            stopSelf()
        }
        return START_NOT_STICKY
    }

    private fun startForegroundCompat() {
        val nm = getSystemService(NotificationManager::class.java)
        val builder: Notification.Builder
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, "Tangkap suara internal", NotificationManager.IMPORTANCE_LOW)
            )
            builder = Notification.Builder(this, CHANNEL_ID)
        } else {
            @Suppress("DEPRECATION")
            builder = Notification.Builder(this)
        }
        val n = builder
            .setContentTitle("DbxComp")
            .setContentText("Memproses suara internal HP")
            .setSmallIcon(R.drawable.ic_stat_dbxcomp)
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
        } else {
            startForeground(NOTIF_ID, n)
        }
    }

    /** Mengembalikan pesan error, atau null kalau penangkapan sudah berjalan. */
    private fun begin(code: Int, data: Intent?, rate: Int): String? {
        if (Build.VERSION.SDK_INT < 29) return "Tangkap suara internal butuh Android 10 ke atas"
        if (data == null) return "Izin tangkap suara tidak diberikan"

        val mpm = getSystemService(MediaProjectionManager::class.java)
        val proj = try {
            mpm.getMediaProjection(code, data)
        } catch (e: Exception) {
            Log.e("DbxComp", "getMediaProjection gagal", e)
            null
        } ?: return "Gagal memulai penangkapan (izin ditolak sistem)"
        projection = proj
        proj.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() {
                report("Penangkapan suara dihentikan oleh sistem")
                main.post { stopSelf() }
            }
        }, main)

        // Tangkap musik/game/suara umum; jangan tangkap suara aplikasi ini sendiri (loop).
        fun config(excludeSelf: Boolean): AudioPlaybackCaptureConfiguration {
            val b = AudioPlaybackCaptureConfiguration.Builder(proj)
                .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                .addMatchingUsage(AudioAttributes.USAGE_GAME)
                .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
            if (excludeSelf) b.excludeUid(Process.myUid())
            return b.build()
        }
        val cfg = try {
            config(true)
        } catch (e: Exception) {
            Log.w("DbxComp", "excludeUid tidak didukung, lanjut tanpa", e)
            config(false)
        }

        captureCfg = cfg
        captureRate = rate
        val rec = openRecord() ?: return "Gagal membuat perekam suara internal (izin mikrofon?)"
        record = rec

        capturing = true
        thread = Thread {
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
            val buf = FloatArray(rate / 100 * 2)  // ~10 ms stereo
            var failures = 0
            while (capturing) {
                val r = record
                val n = if (r != null) r.read(buf, 0, buf.size, AudioRecord.READ_BLOCKING) else -1
                if (n > 0) {
                    failures = 0
                    AudioEngine.nativePushCapture(1, buf, n)
                } else if (n < 0) {
                    // Perekam mati sendiri (mis. sistem merebutnya di latar belakang): buat ulang.
                    Log.e("DbxComp", "AudioRecord.read error $n, mencoba ulang ($failures)")
                    if (++failures > 5) {
                        report("Suara internal berhenti (perekam tidak bisa dipulihkan)")
                        main.post { stopSelf() }
                        break
                    }
                    try {
                        record?.stop()
                    } catch (_: Exception) {
                    }
                    record?.release()
                    record = null
                    try {
                        Thread.sleep(250)
                    } catch (_: InterruptedException) {
                        break
                    }
                    if (capturing) record = openRecord()
                }
            }
        }.also { it.start() }
        return null
    }

    private var captureCfg: AudioPlaybackCaptureConfiguration? = null
    private var captureRate = 48000

    /** Membuat dan memulai AudioRecord untuk penangkapan suara internal; null kalau gagal. */
    private fun openRecord(): AudioRecord? {
        val cfg = captureCfg ?: return null
        val rate = captureRate
        val fmt = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
            .setSampleRate(rate)
            .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
            .build()
        val minBytes = AudioRecord.getMinBufferSize(
            rate, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_FLOAT
        )
        val bytes = max(minBytes, rate / 10 * 2 * 4)  // >= 100 ms
        val rec = try {
            AudioRecord.Builder()
                .setAudioFormat(fmt)
                .setBufferSizeInBytes(bytes)
                .setAudioPlaybackCaptureConfig(cfg)
                .build()
        } catch (e: Exception) {
            Log.e("DbxComp", "AudioRecord gagal dibuat", e)
            return null
        }
        if (rec.state != AudioRecord.STATE_INITIALIZED) {
            rec.release()
            return null
        }
        return try {
            rec.startRecording()
            rec
        } catch (e: Exception) {
            Log.e("DbxComp", "startRecording gagal", e)
            rec.release()
            null
        }
    }

    private fun report(msg: String) {
        main.post { listener?.invoke(msg) }
    }

    override fun onDestroy() {
        capturing = false
        try {
            thread?.join(500)
        } catch (_: InterruptedException) {
        }
        try {
            record?.stop()
        } catch (_: Exception) {
        }
        record?.release()
        record = null
        projection?.stop()
        projection = null
        super.onDestroy()
    }
}
