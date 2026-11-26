package com.example.dbxcomp

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Process
import android.util.Log
import java.io.EOFException

/**
 * Sumber "suara internal ROOT": menjalankan [RootCaptureMain] sebagai root, membaca PCM 16-bit
 * stereo dari stdout-nya, lalu mendorongnya ke mesin native. Tidak memakai MediaProjection,
 * jadi aplikasi yang menolak AudioPlaybackCapture (Spotify, dll) tetap bisa masuk.
 * Berjalan sebagai layanan foreground agar tetap hidup di latar belakang.
 */
class RootCaptureService : Service() {

    companion object {
        private const val HELPER_CLASS = "com.example.dbxcomp.RootCaptureMain"
        private const val CHANNEL_ID = "dbxcomp_root"
        private const val NOTIF_ID = 3
    }

    private val main = Handler(Looper.getMainLooper())
    private var proc: java.lang.Process? = null
    private var thread: Thread? = null
    private val errText = StringBuffer()

    @Volatile
    private var running = false
    private var retries = 0

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent == null) {
            stopSelf()
            return START_NOT_STICKY
        }
        startForegroundCompat()
        val rate = intent.getIntExtra(CaptureService.EXTRA_RATE, 48000)
        val err = begin(rate)
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
                NotificationChannel(CHANNEL_ID, "Suara internal (root)", NotificationManager.IMPORTANCE_LOW)
            )
            builder = Notification.Builder(this, CHANNEL_ID)
        } else {
            @Suppress("DEPRECATION")
            builder = Notification.Builder(this)
        }
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        val n = builder
            .setContentTitle("DbxComp")
            .setContentText("Memproses suara internal HP (root)")
            .setSmallIcon(R.drawable.ic_stat_dbxcomp)
            .setContentIntent(open)
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        } else {
            startForeground(NOTIF_ID, n)
        }
    }

    /** Mengembalikan pesan error, atau null kalau helper root sudah berjalan. */
    private fun begin(rate: Int): String? {
        val apk = packageCodePath
        val cmd = "CLASSPATH='$apk' exec app_process /system/bin $HELPER_CLASS $rate"
        val p = try {
            ProcessBuilder("su", "-c", cmd).start()
        } catch (e: Exception) {
            Log.e("DbxComp", "su gagal dijalankan", e)
            return "Root (su) tidak tersedia: ${e.message}"
        }
        proc = p
        running = true

        Thread {
            try {
                p.errorStream.bufferedReader().forEachLine {
                    Log.w("DbxComp", "root helper: $it")
                    if (errText.length < 2000) errText.append(it).append('\n')
                }
            } catch (_: Exception) {
            }
        }.also { it.isDaemon = true; it.start() }

        thread = Thread { readLoop(p, rate) }.also { it.start() }
        return null
    }

    private fun readLoop(p: java.lang.Process, rate: Int) {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
        val input = p.inputStream
        val bytes = ByteArray(rate / 100 * 4)      // ~10 ms stereo 16-bit
        val buf = FloatArray(rate / 100 * 2)       // stereo float interleaved
        try {
            while (running) {
                var off = 0
                while (off < bytes.size) {
                    val n = input.read(bytes, off, bytes.size - off)
                    if (n < 0) throw EOFException()
                    off += n
                }
                retries = 0  // data mengalir lagi: hitungan percobaan ulang dimulai dari nol
                var i = 0
                for (j in buf.indices) {
                    val s = (bytes[i + 1].toInt() shl 8) or (bytes[i].toInt() and 0xff)
                    buf[j] = s / 32768f
                    i += 2
                }
                AudioEngine.nativePushCapture(2, buf, buf.size)
            }
        } catch (e: Exception) {
            if (running && retries < 3) {
                // Helper root mati sendiri (mis. dibunuh sistem di latar belakang): jalankan ulang.
                retries++
                Log.w("DbxComp", "Helper root berhenti, mencoba ulang ($retries)", e)
                main.postDelayed({
                    if (running) {
                        try {
                            proc?.destroy()
                        } catch (_: Exception) {
                        }
                        errText.setLength(0)
                        val err = begin(rate)
                        if (err != null) {
                            report(err)
                            stopSelf()
                        }
                    }
                }, 400)
            } else if (running) {
                Thread.sleep(150)  // beri waktu thread stderr menangkap pesan
                val detail = errText.toString().trim()
                val msg = when {
                    detail.contains("ERR:") -> "Helper root gagal: " + detail.substringAfter("ERR:").lineSequence().first()
                    detail.isNotEmpty() -> "Helper root berhenti: " + detail.lineSequence().first()
                    else -> "Helper root berhenti (izin root ditolak atau tidak ada)"
                }
                Log.e("DbxComp", msg, e)
                report(msg)
                main.post { stopSelf() }
            }
        }
    }

    private fun report(msg: String) {
        main.post { CaptureService.listener?.invoke(msg) }
    }

    override fun onDestroy() {
        running = false
        try {
            proc?.destroy()
        } catch (_: Exception) {
        }
        proc = null
        // Cadangan: pastikan helper root benar-benar mati.
        Thread {
            try {
                ProcessBuilder("su", "-c", "pkill -f $HELPER_CLASS").start().waitFor()
            } catch (_: Exception) {
            }
        }.also { it.isDaemon = true; it.start() }
        try {
            thread?.join(500)
        } catch (_: InterruptedException) {
        }
        super.onDestroy()
    }
}
