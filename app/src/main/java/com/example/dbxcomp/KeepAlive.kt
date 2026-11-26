package com.example.dbxcomp

import android.content.Context
import android.os.PowerManager

/**
 * Menjaga CPU tetap bangun selama mesin audio berjalan, supaya audio tidak tersendat atau
 * berhenti saat layar mati / aplikasi di latar belakang. Dilepas saat STOP.
 */
object KeepAlive {
    private var lock: PowerManager.WakeLock? = null

    @Synchronized
    fun acquire(context: Context) {
        try {
            if (lock == null) {
                val pm = context.applicationContext.getSystemService(Context.POWER_SERVICE) as PowerManager
                lock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "DbxComp:audio").apply {
                    setReferenceCounted(false)
                }
            }
            if (lock?.isHeld != true) lock?.acquire()
        } catch (_: Exception) {
        }
    }

    @Synchronized
    fun release() {
        try {
            if (lock?.isHeld == true) lock?.release()
        } catch (_: Exception) {
        }
    }
}
