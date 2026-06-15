package com.sunjaycy.goldeneye;

import android.app.NativeActivity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.SystemClock;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;

/**
 * Thin Java shell over NativeActivity.
 *
 * A pure NativeActivity (hasCode=false) came up with a zero-area window
 * (frame=[1080,0][1080,0]) on a dual-screen Adreno handheld, so nothing was
 * visible and its input channel never matched the AInputQueue we drain. Window
 * size/position and immersive flags are Java-side (WindowManager.LayoutParams /
 * View system-UI), which native code cannot set, so we force them here.
 *
 * NativeActivity still loads libge.so (android.app.lib_name=ge meta-data) and
 * runs android_main exactly as before.
 *
 * Boot auto-retry: the guest's cold boot is an intermittent ~42% guest-side race
 * (the main game thread can wedge spinning in the frame-limiter / GPU-completion
 * wait before the first frame; ~42% of launches reach gameplay, the rest hang
 * black). The race is per-process, so a fresh relaunch is an independent ~42%
 * roll. A watchdog turns that into reliable booting: if a launch has not started
 * presenting frames within BOOT_WATCHDOG_MS, kill it and relaunch (up to
 * MAX_BOOT_ATTEMPTS) until one wins the race.
 *
 * The watchdog AND the loading overlay run on DEDICATED THREADS, not the main
 * Looper: when the guest wedges, android_native_app_glue's synchronous command
 * handshake blocks the Java main thread (futex_wait) too, so anything on the main
 * thread would freeze. NativeActivity also takes the window surface for native
 * rendering, so a loading spinner cannot be a View inside this window - it is a
 * separate APPLICATION_PANEL overlay window composited on top of the (black)
 * game surface, shown during boot/retries and removed once frames appear.
 */
public class GoldenEyeActivity extends NativeActivity {
    private static final String TAG = "GEBOOT";
    // A healthy boot creates its swapchain ~2s after launch and reaches a live
    // render (present#65) within ~5s; this window leaves a wide margin (incl. a
    // cold shader cache on first run) while keeping failed-boot retries quick.
    private static final int BOOT_WATCHDOG_MS = 16000;
    private static final int POLL_MS = 2000;
    private static final int MAX_BOOT_ATTEMPTS = 10;
    private static final String ATTEMPT_EXTRA = "ge_boot_attempt";

    // Present counter is logged as "GEGPU present#N" every 64 frames; N>=65 means
    // >=64 frames presented = a real render loop (a wedged boot can emit a lone
    // "present#1" then freeze, so any "present#" is not enough).
    private static final int PRESENT_THRESHOLD = 65;

    private volatile boolean relaunching;
    private volatile boolean stopWatchdog;
    private int attempt;
    private Thread watchdogThread;

    // Loading overlay (own thread; main Looper is unusable while the guest wedges)
    private HandlerThread overlayThread;
    private Handler overlayHandler;
    private View overlayView;
    private TextView overlayText;
    private int dotPhase;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        attempt = getIntent() != null ? getIntent().getIntExtra(ATTEMPT_EXTRA, 0) : 0;

        // Start each launch with a fresh log so the watchdog only sees THIS
        // process's "present#" markers (the runtime appends across launches).
        try {
            File log = new File(getExternalFilesDir(null), "ge.log");
            if (log.exists()) {
                log.delete();
            }
        } catch (Throwable t) {
            // best-effort
        }

        super.onCreate(savedInstanceState);

        // Force the window to fill the display (the dual-screen WM left it 0x0).
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.width = WindowManager.LayoutParams.MATCH_PARENT;
        lp.height = WindowManager.LayoutParams.MATCH_PARENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            lp.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        getWindow().setAttributes(lp);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        hideSystemUi();

        overlayThread = new HandlerThread("ge-overlay");
        overlayThread.start();
        overlayHandler = new Handler(overlayThread.getLooper());

        Log.i(TAG, "boot attempt " + attempt + " -> watchdog armed (" + BOOT_WATCHDOG_MS + "ms)");
        watchdogThread = new Thread(this::bootWatchdogRun, "ge-boot-watchdog");
        watchdogThread.setDaemon(true);
        watchdogThread.start();
    }

    @Override
    public void onAttachedToWindow() {
        super.onAttachedToWindow();
        // The window now has a token (needed for the sub-panel overlay). This runs
        // on the main thread before the native surface is created, i.e. before the
        // guest can wedge it - hand the token to the overlay thread to show the
        // spinner.
        final IBinder token = getWindow().getDecorView().getWindowToken();
        if (token != null && overlayHandler != null) {
            overlayHandler.post(() -> showOverlay(token));
        }
    }

    /**
     * Runs on its own thread. Polls until the guest starts presenting frames (=
     * boot succeeded; remove the spinner) or the deadline passes with no frames (=
     * wedged boot, so relaunch a fresh process to re-roll the startup race).
     */
    private void bootWatchdogRun() {
        long deadline = SystemClock.elapsedRealtime() + BOOT_WATCHDOG_MS;
        while (SystemClock.elapsedRealtime() < deadline) {
            try {
                Thread.sleep(POLL_MS);
            } catch (InterruptedException e) {
                return;
            }
            if (stopWatchdog || relaunching) {
                return;
            }
            if (hasStartedPresenting()) {
                Log.i(TAG, "boot attempt " + attempt + " OK (presenting)");
                hideOverlay();
                return;
            }
        }
        if (stopWatchdog || relaunching) {
            return;
        }
        if (attempt + 1 >= MAX_BOOT_ATTEMPTS) {
            Log.w(TAG, "boot stalled after " + MAX_BOOT_ATTEMPTS + " attempts; giving up");
            return;
        }
        Log.w(TAG, "boot attempt " + attempt + " STALLED (no frames) -> relaunching");
        relaunchSelf(attempt + 1);
    }

    /** True once the native runtime has a LIVE render loop (>= PRESENT_THRESHOLD frames). */
    private boolean hasStartedPresenting() {
        File log = new File(getExternalFilesDir(null), "ge.log");
        if (!log.exists()) {
            return false;
        }
        try (BufferedReader r = new BufferedReader(new FileReader(log))) {
            String line;
            while ((line = r.readLine()) != null) {
                int idx = line.indexOf("present#");
                if (idx < 0) {
                    continue;
                }
                int j = idx + "present#".length();
                int n = 0;
                while (j < line.length() && Character.isDigit(line.charAt(j))) {
                    n = n * 10 + (line.charAt(j) - '0');
                    j++;
                }
                if (n >= PRESENT_THRESHOLD) {
                    return true;
                }
            }
        } catch (Throwable t) {
            // If we can't read it, assume not presenting (safer to retry).
        }
        return false;
    }

    /**
     * Relaunch a fresh process. Called from the watchdog thread while this
     * activity is still the foreground task, so starting the new activity is a
     * foreground launch (not subject to Android 10+ background-launch limits).
     * We then hard-exit because NativeActivity does not cleanly tear down the
     * spinning guest threads on finish(); the queued launch spawns a fresh
     * process. (Same idea as ProcessPhoenix.)
     */
    private void relaunchSelf(int nextAttempt) {
        relaunching = true;
        try {
            Intent intent = new Intent(this, GoldenEyeActivity.class);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            intent.putExtra(ATTEMPT_EXTRA, nextAttempt);
            startActivity(intent);
        } catch (Throwable t) {
            Log.e(TAG, "relaunch failed", t);
        }
        try {
            Thread.sleep(150);
        } catch (InterruptedException ignored) {
        }
        // Hard-exit to kill the wedged guest threads; the queued launch brings up
        // a fresh process (which shows its own spinner) to re-roll the boot race.
        Runtime.getRuntime().exit(0);
    }

    // --- Loading overlay (all View ops on overlayThread) --------------------

    private void showOverlay(IBinder token) {
        if (overlayView != null) {
            return;
        }
        try {
            Context ctx = this;
            FrameLayout root = new FrameLayout(ctx);
            root.setBackgroundColor(Color.BLACK);

            LinearLayout col = new LinearLayout(ctx);
            col.setOrientation(LinearLayout.VERTICAL);
            col.setGravity(Gravity.CENTER_HORIZONTAL);

            ProgressBar spinner = new ProgressBar(ctx);  // default = indeterminate circle
            LinearLayout.LayoutParams sp = new LinearLayout.LayoutParams(dp(56), dp(56));
            col.addView(spinner, sp);

            TextView tv = new TextView(ctx);
            tv.setText("Loading GoldenEye");
            tv.setTextColor(Color.WHITE);
            tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
            tv.setGravity(Gravity.CENTER);
            LinearLayout.LayoutParams tp =
                new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                                              LinearLayout.LayoutParams.WRAP_CONTENT);
            tp.topMargin = dp(20);
            col.addView(tv, tp);
            overlayText = tv;

            FrameLayout.LayoutParams clp =
                new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT,
                                             FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER);
            root.addView(col, clp);
            overlayView = root;

            WindowManager.LayoutParams wlp = new WindowManager.LayoutParams();
            wlp.type = WindowManager.LayoutParams.TYPE_APPLICATION_PANEL;
            wlp.token = token;
            wlp.width = WindowManager.LayoutParams.MATCH_PARENT;
            wlp.height = WindowManager.LayoutParams.MATCH_PARENT;
            wlp.format = PixelFormat.OPAQUE;
            wlp.flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                      | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                      | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                      | WindowManager.LayoutParams.FLAG_FULLSCREEN
                      | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON;
            wlp.gravity = Gravity.CENTER;

            getWindowManager().addView(overlayView, wlp);
            overlayHandler.post(dotRunnable);
        } catch (Throwable t) {
            Log.e(TAG, "overlay show failed", t);
            overlayView = null;
            overlayText = null;
        }
    }

    // Method reference (not an anonymous class) so javac emits no extra .class
    // file - the manual APK build dexes only GoldenEyeActivity.class.
    private final Runnable dotRunnable = this::animateDots;

    private void animateDots() {
        if (overlayView == null || overlayText == null) {
            return;
        }
        dotPhase = (dotPhase + 1) & 3;
        StringBuilder s = new StringBuilder("Loading GoldenEye");
        for (int i = 0; i < dotPhase; i++) {
            s.append('.');
        }
        if (attempt > 0) {
            s.append("\n(retry ").append(attempt).append(')');
        }
        overlayText.setText(s.toString());
        overlayHandler.postDelayed(dotRunnable, 450);
    }

    private void hideOverlay() {
        if (overlayHandler == null) {
            return;
        }
        overlayHandler.post(() -> {
            overlayHandler.removeCallbacks(dotRunnable);
            if (overlayView != null) {
                try {
                    getWindowManager().removeViewImmediate(overlayView);
                } catch (Throwable t) {
                    // ignore
                }
                overlayView = null;
                overlayText = null;
            }
        });
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    @Override
    protected void onDestroy() {
        stopWatchdog = true;
        if (watchdogThread != null) {
            watchdogThread.interrupt();
        }
        hideOverlay();
        if (overlayThread != null) {
            overlayThread.quitSafely();
        }
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemUi();
        }
    }

    @SuppressWarnings("deprecation")
    private void hideSystemUi() {
        View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE
          | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
          | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
          | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
          | View.SYSTEM_UI_FLAG_FULLSCREEN
          | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }
}
