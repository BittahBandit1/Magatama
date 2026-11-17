// The disc importer (src/app/magatama_disc_import.h) for the Android launcher, through libmagatama_import.so
// (src/app/android/magatama_import_jni.cpp).
package dev.magatama;

final class DiscImport {
    static {
        System.loadLibrary("c++_shared");
        System.loadLibrary("magatama_import");
    }

    /** Called on the importing thread; return false to cancel. */
    interface Listener {
        boolean onProgress(long done, long total, String file);
    }

    static final int OK = 0, CANCELLED = 7;

    /** Blocking. Returns "<status> <message>". */
    static native String nativeImport(String image, String discDir, Listener listener);

    /** True when discDir holds the right SLUS_209.11 (hashes 4.5 MB: call it off the UI thread). */
    static native boolean nativeInstalled(String discDir);

    private DiscImport() {}
}
