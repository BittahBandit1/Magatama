// Magatama for Android: SDL3's activity plus the shared-storage access the game needs. The game's files live in
// Documents/Magatama, where the user can copy them over USB or with any file manager; Android only lets an app read
// files that other apps (or USB) put there with "All files access" (Android 11+) or the storage permission (9-10).
// src/app/magatama_android.cpp calls these through JNI on the game thread.
package dev.magatama;

import android.Manifest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.Settings;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import android.content.Context;
import android.view.SurfaceHolder;

import java.io.File;

public class MagatamaActivity extends SDLActivity {
    // The game presents from its own GS thread. Android takes the window surface away in the background (Home, the
    // app switcher, screen off) and gives a new one back on return: tell the GS first, so it lets go of the old
    // surface before SDL does and rebuilds its swap chain on the new one (src/app/magatama_android.cpp).
    static native void nativeSurfaceLost();
    static native void nativeSurfaceReady(android.view.Surface surface, int width, int height);

    static final class GameSurface extends SDLSurface {
        GameSurface(Context context) {
            super(context);
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
            try {
                nativeSurfaceLost();
            } catch (UnsatisfiedLinkError ignored) {
            }
            super.surfaceDestroyed(holder);
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            super.surfaceChanged(holder, format, width, height);
            try {
                nativeSurfaceReady(holder.getSurface(), width, height);
            } catch (UnsatisfiedLinkError ignored) {
            }
        }
    }

    /** True while the game's activity exists: the launcher then hands over to it instead of showing itself. */
    static volatile boolean alive;

    @Override
    protected void onCreate(android.os.Bundle state) {
        alive = true;
        super.onCreate(state);
        pickDisplayMode();
    }

    /** The refresh rate of the display mode this activity asked for (0: none was chosen, the system's current one). */
    private float chosenRefreshRate;

    // The game makes ~60 frames a second and its presentation holds each one for a whole number of refreshes, so it
    // wants a display mode at a multiple of 60 Hz: 120 Hz where there is one (Nova, Odin 2 Portal, Odin 3, RP6, the
    // Thor's top screen), else 60 Hz (RP5, Mini, Flip 2, Classic, Odin 2). Asking for it explicitly also keeps a phone
    // that switches rates by itself (90/144 Hz panels, adaptive "smooth display") on one rate for the whole game. Same
    // resolution only: this never changes the screen's size.
    private void pickDisplayMode() {
        try {
            android.view.Display display = getWindowManager().getDefaultDisplay();
            android.view.Display.Mode current = display.getMode();
            android.view.Display.Mode best = null;
            for (android.view.Display.Mode mode : display.getSupportedModes()) {
                if (mode.getPhysicalWidth() != current.getPhysicalWidth() ||
                    mode.getPhysicalHeight() != current.getPhysicalHeight())
                    continue;
                final float rate = mode.getRefreshRate();
                final float multiple = rate / 60f;
                if (Math.round(multiple) < 1 || Math.abs(multiple - Math.round(multiple)) > 0.01f || rate > 125f)
                    continue;
                if (best == null || rate > best.getRefreshRate())
                    best = mode;
            }
            if (best == null)
                return;
            android.view.WindowManager.LayoutParams params = getWindow().getAttributes();
            params.preferredDisplayModeId = best.getModeId();
            getWindow().setAttributes(params);
            chosenRefreshRate = best.getRefreshRate();
            android.util.Log.i("Magatama", "display mode " + best.getModeId() + ": " + best.getRefreshRate() + " Hz");
        } catch (Exception e) {
            android.util.Log.w("Magatama", "display mode: " + e);
        }
    }

    @Override
    protected void onDestroy() {
        alive = false;
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus)
            getWindow().getDecorView().setSystemUiVisibility(android.view.View.SYSTEM_UI_FLAG_FULLSCREEN |
                android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
                android.view.View.SYSTEM_UI_FLAG_LAYOUT_STABLE | android.view.View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                android.view.View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new GameSurface(context);
    }

    /** True when the app may read and write Documents/Magatama. */
    public boolean magatamaHasStorageAccess() {
        if (Build.VERSION.SDK_INT >= 30)
            return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    /** Opens the system screen that grants it (Android 11+), or asks for the storage permission (9-10). */
    public void magatamaRequestStorageAccess() {
        runOnUiThread(() -> {
            if (Build.VERSION.SDK_INT >= 30) {
                try {
                    startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                             Uri.parse("package:" + getPackageName())));
                } catch (Exception e) {
                    startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                }
            } else {
                requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE,
                                                 Manifest.permission.WRITE_EXTERNAL_STORAGE}, 1);
            }
        });
    }

    /** The display's current refresh rate (Hz): the game's pacing follows it (magatama_android.cpp). */
    public float magatamaDisplayRefreshRate() {
        try {
            return chosenRefreshRate > 0f ? chosenRefreshRate : getWindowManager().getDefaultDisplay().getRefreshRate();
        } catch (Exception e) {
            return 0f;
        }
    }

    /** The shared Documents folder (normally /storage/emulated/0/Documents). */
    public String magatamaDocumentsPath() {
        File documents = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS);
        return documents.getAbsolutePath();
    }
}
