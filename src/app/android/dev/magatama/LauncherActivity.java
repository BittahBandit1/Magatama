// Magatama's launcher on Android: the app's start screen. Imports the user's own disc image (.iso, .chd or .zso),
// picked with the system file picker, into Documents/Magatama/disc (where the game reads it), then starts the game
// (MagatamaActivity). The importer is the same C++ one the iOS and macOS launchers use (DiscImport).
package dev.magatama;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;

public class LauncherActivity extends Activity {
    private static final int PICK_IMAGE = 1;
    private static final int PURPLE = 0xFF8E44D8;

    private TextView status;
    private TextView progressText;
    private ProgressBar progressBar;
    private Button importButton;
    private Button playButton;
    private Button cancelButton;
    private boolean installed;
    private volatile boolean importing;
    private volatile boolean cancelRequested;
    private String message = "";
    private boolean failed;

    // ---------------------------------------------------------------------------------------------- storage access
    // Documents/Magatama needs "All files access" (Android 11+) or the storage permission (9-10), as in MagatamaActivity.
    private boolean hasStorageAccess() {
        if (Build.VERSION.SDK_INT >= 30)
            return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    private void requestStorageAccess() {
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
    }

    private static File discDir() {
        File documents = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS);
        return new File(new File(documents, "Magatama"), "disc");
    }

    // ---------------------------------------------------------------------------------------------- layout
    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private Button button(String text, boolean primary) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(17);
        b.setTextColor(Color.WHITE);
        b.setMinWidth(dp(190));
        b.setPadding(dp(22), dp(12), dp(22), dp(12));
        GradientDrawable shape = new GradientDrawable();
        shape.setCornerRadius(dp(12));
        if (primary) {
            shape.setColor(PURPLE);
        } else {
            shape.setColor(0x22FFFFFF);
            shape.setStroke(dp(1), 0x66FFFFFF);
        }
        b.setBackground(shape);
        b.setStateListAnimator(null);
        return b;
    }

    private TextView text(String value, float size, int color) {
        TextView t = new TextView(this);
        t.setText(value);
        t.setTextSize(size);
        t.setTextColor(color);
        t.setGravity(Gravity.CENTER);
        return t;
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (bringBackGame()) // the game is running: the app icon returns to it
            return;
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setPadding(dp(24), dp(16), dp(24), dp(16));
        root.setBackground(new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                                                new int[] {0xFF0D0D17, 0xFF1F1429}));

        TextView title = text("MAGATAMA", 44, Color.WHITE);
        title.setTypeface(Typeface.create(Typeface.SERIF, Typeface.BOLD));
        title.setLetterSpacing(0.18f);
        root.addView(title);
        root.addView(text("Shin Megami Tensei: Nocturne", 18, 0xB3FFFFFF));

        status = text("", 15, 0x99FFFFFF);
        status.setPadding(0, dp(28), 0, dp(14));
        root.addView(status);

        progressBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progressBar.setMax(1000);
        progressBar.setProgressTintList(android.content.res.ColorStateList.valueOf(PURPLE));
        root.addView(progressBar, new LinearLayout.LayoutParams(dp(420), ViewGroup.LayoutParams.WRAP_CONTENT));
        progressText = text("", 13, 0xB3FFFFFF);
        progressText.setSingleLine(true);
        progressText.setPadding(0, dp(6), 0, dp(6));
        root.addView(progressText);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.CENTER);
        importButton = button("Import Disc", false);
        playButton = button("Play", true);
        cancelButton = button("Cancel", false);
        LinearLayout.LayoutParams gap = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                                                                      ViewGroup.LayoutParams.WRAP_CONTENT);
        gap.setMargins(dp(8), 0, dp(8), 0);
        buttons.addView(importButton, gap);
        buttons.addView(playButton, gap);
        buttons.addView(cancelButton, gap);
        root.addView(buttons);

        TextView hint = text("Your own Nocturne (USA) disc image: .iso, .chd or .zso", 13, 0x73FFFFFF);
        hint.setPadding(0, dp(18), 0, 0);
        root.addView(hint);

        importButton.setOnClickListener(v -> onImport());
        playButton.setOnClickListener(v -> onPlay());
        cancelButton.setOnClickListener(v -> cancelRequested = true);
        setContentView(root);
        getWindow().setNavigationBarColor(0xFF1F1429);
        if (Build.VERSION.SDK_INT >= 28)
            getWindow().getAttributes().layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        hideSystemBars();
        update();
    }

    /** When the game's task is still alive (the launcher was opened again from the app icon), moves it to the front. */
    private boolean bringBackGame() {
        if (MagatamaActivity.alive) { // the game's activity is single-instance: this brings that one forward
            startActivity(new Intent(this, MagatamaActivity.class)
                              .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT));
            finishAndRemoveTask();
            return true;
        }
        try {
            android.app.ActivityManager am = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
            for (android.app.ActivityManager.AppTask task : am.getAppTasks()) {
                android.content.ComponentName base = task.getTaskInfo().baseActivity;
                if (base != null && base.getClassName().equals(MagatamaActivity.class.getName())) {
                    task.moveToFront();
                    finishAndRemoveTask();
                    return true;
                }
            }
        } catch (Exception ignored) {
        }
        return false;
    }

    // Edge to edge: the gradient fills the screen with the system bars hidden.
    private void hideSystemBars() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN |
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus)
            hideSystemBars();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh(); // storage access may have just been granted
    }

    /** Re-checks the installed disc off the UI thread. */
    private void refresh() {
        if (importing)
            return;
        if (!hasStorageAccess()) {
            installed = false;
            update();
            return;
        }
        final String dir = discDir().getAbsolutePath();
        new Thread(() -> {
            final boolean ok = DiscImport.nativeInstalled(dir);
            runOnUiThread(() -> {
                installed = ok;
                update();
            });
        }, "magatama-check").start();
    }

    private void update() {
        final boolean access = hasStorageAccess();
        progressBar.setVisibility(importing ? View.VISIBLE : View.GONE);
        progressText.setVisibility(importing ? View.VISIBLE : View.GONE);
        cancelButton.setVisibility(importing ? View.VISIBLE : View.GONE);
        importButton.setVisibility(importing ? View.GONE : View.VISIBLE);
        playButton.setVisibility(importing ? View.GONE : View.VISIBLE);
        importButton.setText(!access ? "Allow Storage Access" : installed ? "Replace Disc" : "Import Disc");
        playButton.setEnabled(installed);
        playButton.setAlpha(installed ? 1f : 0.4f);
        if (importing) {
            status.setText("Importing…");
            status.setTextColor(0xB3FFFFFF);
        } else if (!message.isEmpty()) {
            status.setText(message);
            status.setTextColor(failed ? 0xFFFF6B6B : 0xFF6BE38A);
        } else if (!access) {
            status.setText("Magatama keeps your disc, saves and settings in Documents/Magatama.\n" +
                           "Allow storage access to import your disc.");
            status.setTextColor(0x99FFFFFF);
        } else {
            status.setText(installed ? "Disc ready" : "No disc imported yet");
            status.setTextColor(installed ? 0xFF6BE38A : 0x99FFFFFF);
        }
        getWindow().getDecorView().setKeepScreenOn(importing);
    }

    // ---------------------------------------------------------------------------------------------- actions
    private void onImport() {
        if (!hasStorageAccess()) {
            requestStorageAccess();
            return;
        }
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        try {
            startActivityForResult(pick, PICK_IMAGE);
        } catch (Exception e) {
            showResult(true, "No file picker is available on this device.");
        }
    }

    private void onPlay() {
        if (!installed)
            return;
        startActivity(new Intent(this, MagatamaActivity.class));
        finishAndRemoveTask();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        refresh();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_IMAGE || resultCode != RESULT_OK || data == null || data.getData() == null)
            return;
        final Uri uri = data.getData();
        final ParcelFileDescriptor descriptor;
        try {
            descriptor = getContentResolver().openFileDescriptor(uri, "r");
        } catch (Exception e) {
            showResult(true, "That file could not be opened.");
            return;
        }
        if (descriptor == null) {
            showResult(true, "That file could not be opened.");
            return;
        }
        final String name = displayName(uri);
        if (name.toLowerCase().endsWith(".cue")) {
            closeQuietly(descriptor);
            showResult(true, "Pick the .bin (or .iso) file rather than the .cue sheet.");
            return;
        }
        // The importer reads the picked file through this process's descriptor; it recognises the format by content.
        final String path = "/proc/self/fd/" + descriptor.getFd();
        final File dir = discDir();
        dir.getParentFile().mkdirs();
        importing = true;
        cancelRequested = false;
        message = "";
        progressBar.setProgress(0);
        progressText.setText(name);
        update();
        new Thread(() -> {
            String result = DiscImport.nativeImport(path, dir.getAbsolutePath(), (done, total, file) -> {
                final int permille = total > 0 ? (int) (done * 1000 / total) : 0;
                runOnUiThread(() -> {
                    progressBar.setProgress(permille);
                    progressText.setText((permille / 10) + "%   " + (file.isEmpty() ? name : file));
                });
                return !cancelRequested;
            });
            closeQuietly(descriptor);
            final int space = result.indexOf(' ');
            int code = -1;
            try {
                code = Integer.parseInt(space > 0 ? result.substring(0, space) : result);
            } catch (NumberFormatException ignored) {
            }
            final int finalCode = code;
            final String text = space > 0 ? result.substring(space + 1) : "The importer stopped unexpectedly.";
            runOnUiThread(() -> {
                importing = false;
                showResult(finalCode != DiscImport.OK, text);
                refresh();
            });
        }, "magatama-import").start();
    }

    private void showResult(boolean isFailure, String text) {
        failed = isFailure;
        message = text;
        update();
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0))
                return c.getString(0);
        } catch (Exception ignored) {
        }
        String last = uri.getLastPathSegment();
        return last != null ? last : "disc image";
    }

    private static void closeQuietly(ParcelFileDescriptor descriptor) {
        try {
            descriptor.close();
        } catch (Exception ignored) {
        }
    }

    @Override
    public void onBackPressed() {
        if (importing)
            cancelRequested = true; // stay until the importer has cleaned up
        else
            super.onBackPressed();
    }
}
