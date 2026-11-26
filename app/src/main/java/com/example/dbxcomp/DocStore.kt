package com.example.dbxcomp

import android.content.ContentUris
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.MediaStore
import android.util.Log
import java.io.File

/**
 * Menyimpan file teks (JSON) di Documents/DbxComp/<sub> supaya terlihat di aplikasi Files
 * dan bisa dibagikan. Android 10+ memakai MediaStore (tanpa izin tambahan); Android 9 ke
 * bawah memakai folder Documents milik aplikasi.
 *
 * Catatan: lewat MediaStore, aplikasi hanya bisa melihat file yang dibuatnya sendiri.
 * Setelah aplikasi dipasang ulang dengan tanda tangan berbeda, file lama tetap ada di folder
 * tetapi tidak muncul di daftar; pakai tombol Impor untuk memasukkannya kembali.
 */
object DocStore {
    private const val TAG = "DbxComp"
    private const val ROOT = "Documents/DbxComp"

    private fun relPath(sub: String) = "$ROOT/$sub"

    private fun collection(): Uri =
        MediaStore.Files.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY)

    private fun legacyDir(context: Context, sub: String): File {
        val base = context.getExternalFilesDir("Documents") ?: context.filesDir
        val dir = File(base, "DbxComp/$sub")
        dir.mkdirs()
        return dir
    }

    private fun findUri(context: Context, sub: String, fileName: String): Uri? {
        val c = context.contentResolver.query(
            collection(),
            arrayOf(MediaStore.MediaColumns._ID),
            "${MediaStore.MediaColumns.RELATIVE_PATH} LIKE ? AND " +
                "${MediaStore.MediaColumns.DISPLAY_NAME} = ?",
            arrayOf(relPath(sub) + "%", fileName),
            null
        ) ?: return null
        return c.use {
            if (it.moveToFirst()) ContentUris.withAppendedId(collection(), it.getLong(0)) else null
        }
    }

    /** Menulis (atau menimpa) file teks. Mengembalikan lokasi yang bisa ditampilkan, atau null. */
    fun write(context: Context, sub: String, fileName: String, text: String): String? {
        try {
            val bytes = text.toByteArray(Charsets.UTF_8)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                val resolver = context.contentResolver
                var uri = findUri(context, sub, fileName)
                if (uri == null) {
                    val cv = ContentValues()
                    cv.put(MediaStore.MediaColumns.DISPLAY_NAME, fileName)
                    cv.put(MediaStore.MediaColumns.MIME_TYPE, "application/json")
                    cv.put(MediaStore.MediaColumns.RELATIVE_PATH, relPath(sub))
                    uri = resolver.insert(collection(), cv)
                }
                if (uri == null) return null
                val out = resolver.openOutputStream(uri, "wt") ?: return null
                out.use { it.write(bytes) }
                return relPath(sub) + "/" + fileName
            } else {
                val f = File(legacyDir(context, sub), fileName)
                f.writeBytes(bytes)
                return f.absolutePath
            }
        } catch (e: Exception) {
            Log.e(TAG, "Gagal menulis $fileName", e)
            return null
        }
    }

    fun read(context: Context, sub: String, fileName: String): String? {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                val uri = findUri(context, sub, fileName) ?: return null
                val input = context.contentResolver.openInputStream(uri) ?: return null
                return input.use { String(it.readBytes(), Charsets.UTF_8) }
            } else {
                val f = File(legacyDir(context, sub), fileName)
                return if (f.exists()) f.readText(Charsets.UTF_8) else null
            }
        } catch (e: Exception) {
            Log.e(TAG, "Gagal membaca $fileName", e)
            return null
        }
    }

    /** Daftar nama file .json di folder [sub], terurut. */
    fun list(context: Context, sub: String): List<String> {
        val out = ArrayList<String>()
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                val c = context.contentResolver.query(
                    collection(),
                    arrayOf(MediaStore.MediaColumns.DISPLAY_NAME),
                    "${MediaStore.MediaColumns.RELATIVE_PATH} LIKE ?",
                    arrayOf(relPath(sub) + "%"),
                    null
                )
                if (c != null) {
                    c.use {
                        while (it.moveToNext()) {
                            val n = it.getString(0)
                            if (n != null && n.endsWith(".json")) out.add(n)
                        }
                    }
                }
            } else {
                val files = legacyDir(context, sub).listFiles()
                if (files != null) {
                    for (f in files) if (f.name.endsWith(".json")) out.add(f.name)
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Gagal membaca daftar $sub", e)
        }
        return out.distinct().sorted()
    }

    fun delete(context: Context, sub: String, fileName: String): Boolean {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                val uri = findUri(context, sub, fileName) ?: return false
                return context.contentResolver.delete(uri, null, null) > 0
            } else {
                return File(legacyDir(context, sub), fileName).delete()
            }
        } catch (e: Exception) {
            Log.e(TAG, "Gagal menghapus $fileName", e)
            return false
        }
    }

    /**
     * Membuka menu bagikan. Android 10+: file-nya langsung dilampirkan. Android 9 ke bawah:
     * isi JSON dibagikan sebagai teks.
     */
    fun share(context: Context, sub: String, fileName: String, text: String) {
        val send = Intent(Intent.ACTION_SEND)
        var uri: Uri? = null
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) uri = findUri(context, sub, fileName)
        if (uri != null) {
            send.type = "application/json"
            send.putExtra(Intent.EXTRA_STREAM, uri)
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } else {
            send.type = "text/plain"
            send.putExtra(Intent.EXTRA_TEXT, text)
        }
        send.putExtra(Intent.EXTRA_SUBJECT, fileName)
        context.startActivity(Intent.createChooser(send, "Bagikan $fileName"))
    }
}
