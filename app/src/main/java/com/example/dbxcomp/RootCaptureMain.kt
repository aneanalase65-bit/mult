package com.example.dbxcomp

import android.media.AudioFormat
import android.media.AudioRecord
import android.os.Looper
import java.io.FileDescriptor
import java.io.FileOutputStream
import kotlin.math.abs
import kotlin.math.max
import kotlin.system.exitProcess

/**
 * Helper yang dijalankan sebagai ROOT lewat `su -c app_process ...` (BUKAN di dalam aplikasi).
 * Merekam campuran semua suara keluaran HP (AudioSource.REMOTE_SUBMIX) lalu menulis PCM 16-bit
 * stereo little-endian mentah ke stdout. Aplikasi membaca stdout itu di [RootCaptureService].
 * Pesan error ditulis ke stderr dengan awalan "ERR:".
 *
 * Pemulihan: saat pemutar (mis. YouTube) pause atau ganti video, sesi audionya ditutup dan
 * perekam REMOTE_SUBMIX di sebagian HP macet atau tidak menerima suara baru. Karena itu perekam
 * dibuat ulang bila (a) senyap terus-menerus beberapa saat, (b) read mengembalikan 0 / error,
 * atau (c) read tidak kembali sama sekali (dijaga thread pengawas).
 */
object RootCaptureMain {
    private const val SOURCE_REMOTE_SUBMIX = 8  // MediaRecorder.AudioSource.REMOTE_SUBMIX (hidden)
    private const val SILENT_REFRESH_MS = 400L   // senyap selama ini -> buat ulang perekam
    private const val ZERO_REFRESH_MS = 600L     // read terus 0 selama ini -> buat ulang
    private const val STALL_MS = 1500L           // read tidak kembali selama ini -> pengawas turun tangan
    private const val SILENCE_LEVEL = 8          // |sampel| <= 8 (dari 32768) dianggap senyap

    @Volatile
    private var rec: AudioRecord? = null

    @Volatile
    private var lastReturnMs = 0L

    private fun openRecord(rate: Int, bytes: Int): AudioRecord? {
        return try {
            val r = AudioRecord(
                SOURCE_REMOTE_SUBMIX, rate, AudioFormat.CHANNEL_IN_STEREO,
                AudioFormat.ENCODING_PCM_16BIT, bytes
            )
            if (r.state != AudioRecord.STATE_INITIALIZED) {
                r.release()
                null
            } else {
                r.startRecording()
                r
            }
        } catch (_: Throwable) {
            null
        }
    }

    private fun closeRecord(r: AudioRecord?) {
        try {
            r?.stop()
        } catch (_: Throwable) {
        }
        try {
            r?.release()
        } catch (_: Throwable) {
        }
    }

    private fun isSilent(buf: ByteArray, n: Int): Boolean {
        var i = 0
        while (i + 1 < n) {
            val s = (buf[i + 1].toInt() shl 8) or (buf[i].toInt() and 0xff)
            if (abs(s.toShort().toInt()) > SILENCE_LEVEL) return false
            i += 2
        }
        return true
    }

    @JvmStatic
    fun main(args: Array<String>) {
        val rate = args.getOrNull(0)?.toIntOrNull() ?: 48000
        try {
            Looper.prepareMainLooper()
        } catch (_: Throwable) {
        }

        // FileOutputStream (bukan System.out) supaya error pipa putus benar-benar dilempar.
        val out = FileOutputStream(FileDescriptor.out)
        val minBytes = AudioRecord.getMinBufferSize(
            rate, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT
        )
        val bytes = max(minBytes, rate / 10 * 4)  // >= 100 ms

        val first = try {
            AudioRecord(
                SOURCE_REMOTE_SUBMIX, rate, AudioFormat.CHANNEL_IN_STEREO,
                AudioFormat.ENCODING_PCM_16BIT, bytes
            )
        } catch (e: Throwable) {
            System.err.println("ERR:REMOTE_SUBMIX gagal dibuat: $e")
            exitProcess(2)
        }
        if (first.state != AudioRecord.STATE_INITIALIZED) {
            System.err.println("ERR:REMOTE_SUBMIX tidak siap (ditolak sistem/pabrikan HP)")
            exitProcess(3)
        }
        try {
            first.startRecording()
        } catch (e: Throwable) {
            System.err.println("ERR:startRecording gagal: $e")
            exitProcess(4)
        }
        rec = first
        lastReturnMs = System.currentTimeMillis()

        // Pengawas: kalau read macet total (tidak kembali), hentikan perekam supaya read lepas.
        Thread {
            while (true) {
                try {
                    Thread.sleep(500)
                } catch (_: InterruptedException) {
                    return@Thread
                }
                if (System.currentTimeMillis() - lastReturnMs > STALL_MS) {
                    try {
                        rec?.stop()
                    } catch (_: Throwable) {
                    }
                }
            }
        }.also { it.isDaemon = true; it.start() }

        val buf = ByteArray(rate / 100 * 4)  // ~10 ms stereo 16-bit
        val silentLimitBytes = rate.toLong() * 4 * SILENT_REFRESH_MS / 1000
        var silentBytes = 0L
        var zeroSince = 0L
        var openFails = 0
        var readErrors = 0

        fun reopen() {
            closeRecord(rec)
            rec = null
            val r = openRecord(rate, bytes)
            if (r != null) {
                rec = r
                openFails = 0
            } else {
                openFails++
            }
            lastReturnMs = System.currentTimeMillis()
        }

        try {
            while (true) {
                val cur = rec
                val n = if (cur != null) cur.read(buf, 0, buf.size) else -1
                lastReturnMs = System.currentTimeMillis()
                if (n > 0) {
                    out.write(buf, 0, n)  // pipa putus -> IOException -> keluar
                    zeroSince = 0L
                    readErrors = 0
                    if (isSilent(buf, n)) {
                        silentBytes += n
                        if (silentBytes >= silentLimitBytes) {
                            silentBytes = 0L
                            reopen()  // senyap lama: segarkan, siapa tahu sesi lama sudah mati
                        }
                    } else {
                        silentBytes = 0L
                    }
                } else if (n == 0) {
                    val now = System.currentTimeMillis()
                    if (zeroSince == 0L) zeroSince = now
                    if (now - zeroSince >= ZERO_REFRESH_MS) {
                        zeroSince = 0L
                        reopen()
                    } else {
                        Thread.sleep(10)
                    }
                } else {
                    // Error baca (mis. perekam dihentikan pengawas atau sistem): buat ulang.
                    readErrors++
                    Thread.sleep(50)
                    reopen()
                    if (openFails > 10 || readErrors > 40) {
                        System.err.println("ERR:REMOTE_SUBMIX tidak bisa dipulihkan (error $n)")
                        break
                    }
                }
            }
        } catch (_: Throwable) {
            // aplikasi sudah berhenti membaca: selesai
        } finally {
            closeRecord(rec)
        }
        exitProcess(0)
    }
}
