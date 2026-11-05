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

import java.io.File;

public class MagatamaActivity extends SDLActivity {
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

    /** The shared Documents folder (normally /storage/emulated/0/Documents). */
    public String magatamaDocumentsPath() {
        File documents = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS);
        return documents.getAbsolutePath();
    }
}
