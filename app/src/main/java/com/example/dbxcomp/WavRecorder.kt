package com.example.dbxcomp

import android.content.ContentValues
import android.content.Context
import android.os.Build
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.provider.MediaStore
import android.util.Log
import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.atomic.AtomicLong
import org.json.JSONObject
import kotlin.math.abs
import kotlin.math.log10
import kotlin.math.sqrt

/**
 * Merekam hasil akhir (setelah compressor + limiter) ke file WAV 16-bit.
 * Android 10+: disimpan di Music/DbxComp (terlihat di aplikasi Files dan pemutar musik).
 * Android 9 ke bawah: di folder Music milik aplikasi.
 *
 * twoChannels = true menulis kedua nilai tiap pasangan dari mesin audio:
 * - input stereo: kiri dan kanan hasil olahan (stereo asli);
 * - input mono + mode A/B: kiri = suara asli (setelah input gain), kanan = hasil olahan,
 *   untuk dibandingkan di editor audio.
 * twoChannels = false menulis hasil olahan saja (mono).
 */
class WavRecorder(
    private val context: Context,
    private val onDone: (String?) -> Unit
) {
    private class Sink(
        val channel: FileChannel, val location: String, val base: String, val close: () -> Unit
    )

    /** Statistik sederhana satu kanal WAV: puncak, RMS, dan RMS jendela 100 ms terendah. */
    private class ChStats(private val windowFrames: Int) {
        var peak = 0f
        var sumSq = 0.0
        var count = 0L
        private var winSq = 0.0
        private var winN = 0
        private val windowDb = ArrayList<Float>()

        fun add(x: Float) {
            val a = abs(x)
            if (a > peak) peak = a
            val sq = x.toDouble() * x.toDouble()
            sumSq += sq
            count++
            winSq += sq
            winN++
            if (winN >= windowFrames) {
                windowDb.add(db(sqrt(winSq / winN)))
                winSq = 0.0
                winN = 0
            }
        }

        fun json(name: String): JSONObject {
            val o = JSONObject()
            o.put("kanal", name)
            o.put("puncak_dbfs", r1(db(peak.toDouble())))
            if (count > 0) o.put("rms_dbfs", r1(db(sqrt(sumSq / count))))
            if (windowDb.isNotEmpty()) {
                val sorted = windowDb.sorted()
                o.put("rms_terendah_100ms_dbfs", r1(sorted.first()))
                val i10 = (sorted.size * 0.1).toInt().coerceAtMost(sorted.size - 1)
                o.put("rms_persentil10_100ms_dbfs", r1(sorted[i10]))
            }
            return o
        }

        private fun db(x: Double): Float = if (x < 1e-9) -180f else (20.0 * log10(x)).toFloat()
        private fun r1(v: Float): Double = String.format(Locale.US, "%.1f", v).toDouble()
    }

    @Volatile
    var isRecording = false
        private set

    /** Diisi Activity: potret setelan dan perangkat saat rekaman dimulai (dipanggil di thread utama). */
    var infoSnapshot: (() -> JSONObject?)? = null

    /** Lokasi file info pendamping rekaman terakhir (null kalau gagal ditulis). */
    @Volatile
    var lastInfoLocation: String? = null
        private set

    private val main = Handler(Looper.getMainLooper())
    private val frames = AtomicLong(0)
    private var sampleRate = 48000
    private var thread: Thread? = null

    val elapsedMs: Long get() = frames.get() * 1000L / sampleRate

    fun start(sampleRate: Int, twoChannels: Boolean): Boolean {
        if (isRecording || thread?.isAlive == true) return false
        this.sampleRate = if (sampleRate > 0) sampleRate else 48000
        frames.set(0)
        val sink = openSink() ?: return false
        val channels = if (twoChannels) 2 else 1
        val rate = this.sampleRate
        lastInfoLocation = null
        val snapshot: JSONObject? = try {
            infoSnapshot?.invoke()
        } catch (e: Exception) {
            Log.e("DbxComp", "Gagal membuat potret setelan", e)
            null
        }
        val stats = arrayOf(ChStats(rate / 10), ChStats(rate / 10))

        isRecording = true
        AudioEngine.nativeSetRecording(true)

        thread = Thread {
            var result: String? = null
            try {
                writeFully(sink.channel, wavHeader(rate, channels, 0L))
                val buf = FloatArray(8192)
                val bytes = ByteBuffer.allocate(buf.size * 2).order(ByteOrder.LITTLE_ENDIAN)
                var dataBytes = 0L
                while (true) {
                    val n = AudioEngine.nativeReadRecorded(buf)
                    if (n > 0) {
                        bytes.clear()
                        var i = 0
                        while (i + 1 < n) {
                            if (twoChannels) bytes.putShort(toPcm(buf[i]))  // asli
                            bytes.putShort(toPcm(buf[i + 1]))            // hasil
                            stats[0].add(buf[i])
                            stats[1].add(buf[i + 1])
                            i += 2
                        }
                        bytes.flip()
                        dataBytes += bytes.limit()
                        writeFully(sink.channel, bytes)
                        frames.addAndGet((n / 2).toLong())
                    } else if (!isRecording) {
                        break
                    } else {
                        Thread.sleep(15)
                    }
                }
                // Tulis ulang header dengan ukuran akhir.
                sink.channel.position(0)
                writeFully(sink.channel, wavHeader(rate, channels, dataBytes))
                result = sink.location
            } catch (e: Exception) {
                Log.e("DbxComp", "Rekaman gagal", e)
            } finally {
                isRecording = false
                AudioEngine.nativeSetRecording(false)
                try {
                    sink.close()
                } catch (e: Exception) {
                    Log.e("DbxComp", "Gagal menutup file", e)
                    result = null
                }
                if (result != null) {
                    writeInfo(sink.base, snapshot, rate, channels, twoChannels, stats)
                }
                val r = result
                main.post { onDone(r) }
            }
        }.also { it.start() }
        return true
    }

    /** Berhenti merekam; file diselesaikan di thread latar lalu [onDone] dipanggil. */
    fun stop() {
        if (!isRecording) return
        isRecording = false
        AudioEngine.nativeSetRecording(false)
    }

    private fun writeInfo(
        base: String, snapshot: JSONObject?, rate: Int, channels: Int,
        twoChannels: Boolean, stats: Array<ChStats>
    ) {
        try {
            val info = JSONObject()
            info.put("format", "dbxcomp-rekaman-info")
            info.put("formatVersion", 1)
            info.put("fileWav", "$base.wav")
            info.put("dibuatPada", PresetStore.nowIso())
            if (snapshot != null) {
                val it = snapshot.keys()
                while (it.hasNext()) {
                    val k = it.next()
                    info.put(k, snapshot.get(k))
                }
            }
            val st = JSONObject()
            st.put("durasi_detik", frames.get().toDouble() / rate)
            st.put("sampleRate", rate)
            st.put("channelWav", channels)
            val ch = org.json.JSONArray()
            if (twoChannels) {
                ch.put(stats[0].json("kiri"))
                ch.put(stats[1].json("kanan"))
            } else {
                ch.put(stats[1].json("mono"))
            }
            st.put("kanal", ch)
            info.put("statistik", st)
            lastInfoLocation =
                DocStore.write(context, PresetStore.SUB_REKAMAN, "$base.json", info.toString(2))
        } catch (e: Exception) {
            Log.e("DbxComp", "Gagal menulis file info rekaman", e)
        }
    }

    private fun openSink(): Sink? {
        val base = "DbxComp_" + SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
        val name = "$base.wav"
        return try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                val resolver = context.contentResolver
                val cv = ContentValues().apply {
                    put(MediaStore.MediaColumns.DISPLAY_NAME, name)
                    put(MediaStore.MediaColumns.MIME_TYPE, "audio/wav")
                    put(MediaStore.MediaColumns.RELATIVE_PATH, "Music/DbxComp")
                    put(MediaStore.MediaColumns.IS_PENDING, 1)
                }
                val uri = resolver.insert(
                    MediaStore.Audio.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY), cv
                ) ?: return null
                val pfd = resolver.openFileDescriptor(uri, "rw") ?: return null
                val ch = ParcelFileDescriptor.AutoCloseOutputStream(pfd).channel
                Sink(ch, "Music/DbxComp/$name", base) {
                    ch.close()
                    val done = ContentValues().apply {
                        put(MediaStore.MediaColumns.IS_PENDING, 0)
                    }
                    resolver.update(uri, done, null, null)
                }
            } else {
                val dir = context.getExternalFilesDir(Environment.DIRECTORY_MUSIC)
                    ?: context.filesDir
                dir.mkdirs()
                val f = File(dir, name)
                val raf = RandomAccessFile(f, "rw")
                Sink(raf.channel, f.absolutePath, base) { raf.close() }
            }
        } catch (e: Exception) {
            Log.e("DbxComp", "Gagal membuat file rekaman", e)
            null
        }
    }

    private fun toPcm(x: Float): Short = (x.coerceIn(-1f, 1f) * 32767f).toInt().toShort()

    private fun writeFully(ch: FileChannel, bb: ByteBuffer) {
        while (bb.hasRemaining()) ch.write(bb)
    }

    private fun wavHeader(sampleRate: Int, channels: Int, dataBytes: Long): ByteBuffer {
        val bb = ByteBuffer.allocate(44).order(ByteOrder.LITTLE_ENDIAN)
        bb.put("RIFF".toByteArray(Charsets.US_ASCII))
        bb.putInt((36 + dataBytes).toInt())
        bb.put("WAVE".toByteArray(Charsets.US_ASCII))
        bb.put("fmt ".toByteArray(Charsets.US_ASCII))
        bb.putInt(16)
        bb.putShort(1)  // PCM
        bb.putShort(channels.toShort())
        bb.putInt(sampleRate)
        bb.putInt(sampleRate * channels * 2)
        bb.putShort((channels * 2).toShort())
        bb.putShort(16)
        bb.put("data".toByteArray(Charsets.US_ASCII))
        bb.putInt(dataBytes.toInt())
        bb.flip()
        return bb
    }
}
