package com.example.dbxcomp

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder

/**
 * Layanan foreground bertipe microphone. Tugasnya hanya satu: menjaga proses tetap hidup dan
 * mengizinkan akses mikrofon/line-in/USB saat aplikasi di latar belakang (layar mati atau
 * pindah aplikasi). Pemrosesan audio sendiri tetap di mesin native (Oboe).
 */
class MicService : Service() {

    companion object {
        private const val CHANNEL_ID = "dbxcomp_mic"
        private const val NOTIF_ID = 2
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val nm = getSystemService(NotificationManager::class.java)
        val builder: Notification.Builder
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, "Input mikrofon / line-in", NotificationManager.IMPORTANCE_LOW)
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
            .setContentText("Memproses input mikrofon / line-in")
            .setSmallIcon(R.drawable.ic_stat_dbxcomp)
            .setContentIntent(open)
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE)
        } else {
            startForeground(NOTIF_ID, n)
        }
        return START_NOT_STICKY
    }
}
