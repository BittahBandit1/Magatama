// Runs GameBuilder in a foreground service, so the build (tens of minutes on a phone) goes on while the screen is off
// or another app is in front, with its progress in a notification. LauncherActivity starts it and shows the same
// progress through BuildService.watch().
package dev.magatama;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;

import java.io.File;

public class BuildService extends Service {
    /** What the launcher shows while a build runs (all calls on the build's threads). */
    interface Watcher {
        void changed();
    }

    static final class State {
        volatile boolean running;
        volatile int step = -1;
        volatile String stepName = "";
        volatile int done, total;
        volatile String what = "";
        volatile String error = "";
        volatile boolean finished;
    }

    static final State state = new State();
    private static volatile Watcher watcher;
    private static volatile GameBuilder builder;
    private static final int NOTIFICATION = 1;
    private static final String CHANNEL = "build";

    static void watch(Watcher w) {
        watcher = w;
    }

    private static boolean working; // the build thread exists (state.running is already true while it starts)

    static void start(Context context) {
        if (state.running)
            return;
        state.running = true;
        state.finished = false;
        state.error = "";
        state.step = -1;
        final Intent intent = new Intent(context, BuildService.class);
        if (Build.VERSION.SDK_INT >= 26)
            context.startForegroundService(intent);
        else
            context.startService(intent);
    }

    static void cancel() {
        final GameBuilder b = builder;
        if (b != null)
            b.cancel();
    }

    private static void notifyWatcher() {
        final Watcher w = watcher;
        if (w != null)
            w.changed();
    }

    private PowerManager.WakeLock wakeLock;
    private long lastNotified;

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationManager nm = getSystemService(NotificationManager.class);
            nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Building the game",
                                                                 NotificationManager.IMPORTANCE_LOW));
        }
        final Notification n = notification("Starting…", 0, 0);
        if (Build.VERSION.SDK_INT >= 34)
            startForeground(NOTIFICATION, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        else
            startForeground(NOTIFICATION, n);
        if (working)
            return START_NOT_STICKY;
        working = true;
        state.running = true;
        final PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "magatama:build");
        wakeLock.acquire(4 * 60 * 60 * 1000L);
        final File documents = android.os.Environment.getExternalStoragePublicDirectory(
            android.os.Environment.DIRECTORY_DOCUMENTS);
        final File disc = new File(new File(documents, "Magatama"), "disc");
        new Thread(() -> {
            final GameBuilder b = new GameBuilder(this, disc, new GameBuilder.Listener() {
                @Override
                public void step(int index, String name) {
                    state.step = index;
                    state.stepName = name;
                    state.done = 0;
                    state.total = 0;
                    state.what = "";
                    update(true);
                }

                @Override
                public void progress(int done, int total, String what) {
                    state.done = done;
                    state.total = total;
                    state.what = what;
                    update(false);
                }

                @Override
                public void log(String line) {
                }
            });
            builder = b;
            try {
                b.run();
            } catch (GameBuilder.BuildException e) {
                state.error = e.getMessage();
            } catch (Throwable t) {
                state.error = t.toString();
            }
            builder = null;
            working = false;
            state.running = false;
            state.finished = true;
            if (wakeLock.isHeld())
                wakeLock.release();
            notifyWatcher();
            stopForeground(true);
            if (!state.error.isEmpty() && !state.error.equals("cancelled")) {
                NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
                nm.notify(NOTIFICATION + 1, notification("The build failed. Open Magatama for details.", 0, 0));
            } else if (state.error.isEmpty()) {
                NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
                nm.notify(NOTIFICATION + 1, notification("Magatama is ready to play.", 0, 0));
            }
            stopSelf();
        }, "magatama-build").start();
        return START_NOT_STICKY;
    }

    private void update(boolean force) {
        notifyWatcher();
        final long now = System.currentTimeMillis();
        if (!force && now - lastNotified < 1000)
            return;
        lastNotified = now;
        final String text = state.stepName + (state.total > 0 ? "  " + (state.done * 100 / state.total) + "%" : "");
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        nm.notify(NOTIFICATION, notification(text, state.done, state.total));
    }

    private Notification notification(String text, int done, int total) {
        final Notification.Builder b = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(this, CHANNEL)
                                                                     : new Notification.Builder(this);
        final PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, LauncherActivity.class),
                                                             PendingIntent.FLAG_IMMUTABLE);
        b.setContentTitle("Magatama").setContentText(text).setSmallIcon(android.R.drawable.stat_sys_download)
            .setContentIntent(open).setOnlyAlertOnce(true);
        if (total > 0)
            b.setProgress(total, done, false);
        return b.build();
    }
}
