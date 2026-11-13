package app.menhir

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import android.util.Log

/**
 * Keeps the process alive, with a notification, while a Mac runs (or is paused in memory) or macOS is
 * installing. It does no work itself: the engine's threads do. [refresh] starts or stops it to match.
 */
class MenhirService : Service() {
    companion object {
        private const val CHANNEL = "menhir"
        private const val ID = 1
        @Volatile private var running = false
        @android.annotation.SuppressLint("StaticFieldLeak") // the application context
        private lateinit var app: Context

        fun init(context: Context) {
            app = context.applicationContext
            val channel = NotificationChannel(CHANNEL, "Running Mac", NotificationManager.IMPORTANCE_LOW)
            channel.setShowBadge(false)
            app.getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        }

        private fun needed() = Installer.busy || Session.active

        /** Start or stop the service to match what is going on. Call from the foreground to start it. */
        fun refresh(context: Context) {
            val c = context.applicationContext
            if (needed()) {
                if (!running) {
                    try {
                        c.startForegroundService(Intent(c, MenhirService::class.java))
                    } catch (e: Exception) {
                        // Only from the foreground: Android refuses a start from the background.
                        Log.w("menhir", "the foreground service could not start", e)
                    }
                } else update()
            } else if (running) {
                c.stopService(Intent(c, MenhirService::class.java))
            }
        }

        /** The notification again, with what is happening now. */
        fun update() {
            if (!running) return
            app.getSystemService(NotificationManager::class.java).notify(ID, notification(app))
        }

        private fun notification(c: Context): Notification {
            val open = PendingIntent.getActivity(
                c, 0, Intent(c, if (Session.active) SessionActivity::class.java else MainActivity::class.java)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )
            val b = Notification.Builder(c, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setContentIntent(open)
                .setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE)
            val s = Installer.status.value
            if (s is InstallStatus.Working) {
                b.setContentTitle("Installing macOS on ${s.macName}")
                b.setContentText(listOf(s.phase, s.detail).filter { it.isNotEmpty() }.joinToString(" · "))
                val p = s.progress
                if (p != null) b.setProgress(1000, (p * 1000).toInt(), false) else b.setProgress(0, 0, true)
            } else {
                b.setSmallIcon(android.R.drawable.ic_media_play)
                b.setContentTitle(Session.title())
                b.setContentText(if (Engine.state() == Engine.PAUSED) "Paused" else "Running")
            }
            return b.build()
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        running = true
        startForeground(ID, notification(this), ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        if (!needed()) stopSelf()
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        running = false
        super.onDestroy()
    }
}
