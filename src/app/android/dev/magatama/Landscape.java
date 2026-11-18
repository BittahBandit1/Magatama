// Keeps Magatama in landscape and stops it turning over while it runs. The manifest starts each activity in
// sensorLandscape (either landscape, whichever way the device is held); as soon as the activity is in landscape it is
// locked to that rotation, so tilting a handheld or setting it down never flips the picture.
package dev.magatama;

import android.app.Activity;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;

final class Landscape {
    private Landscape() {}

    static void lock(Activity a) {
        final boolean landscape = a.getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE;
        final int want = landscape ? ActivityInfo.SCREEN_ORIENTATION_LOCKED : ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE;
        if (a.getRequestedOrientation() != want)
            a.setRequestedOrientation(want);
    }
}
