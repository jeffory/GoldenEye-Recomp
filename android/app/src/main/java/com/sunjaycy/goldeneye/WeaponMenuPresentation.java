package com.sunjaycy.goldeneye;

import android.app.Presentation;
import android.content.Context;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;

/**
 * The weapon menu shown on the AYN Thor's secondary display.
 *
 * Per the dual-screen spike, the Thor exposes its 3.92" bottom panel as a normal
 * Android presentation Display (DisplayManager.getDisplays()), so we host a
 * SurfaceView inside a Presentation on that display and hand its Surface to
 * native code (ANativeWindow). The native ReXGlue secondary surface renders the
 * ImGui weapon menu into it; we only forward the Surface lifecycle and touch.
 *
 * Nothing here draws the menu in Java -- the View is just a Surface provider.
 */
final class WeaponMenuPresentation extends Presentation {
    private static final String TAG = "GEDS";

    private final GoldenEyeActivity host;
    private SurfaceView surfaceView;

    WeaponMenuPresentation(Context outerContext, Display display, GoldenEyeActivity host) {
        super(outerContext, display);
        this.host = host;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Keep the panel awake while the menu is up.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // Never take input focus. A Presentation window is focusable by
        // default, so the first tap on the bottom panel moved window focus --
        // and with it ALL gamepad/button input -- off the game until the top
        // screen was tapped again. Non-focusable windows still receive touch,
        // which is the only input this menu uses (forwarded to native below),
        // so the game keeps focus permanently.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);

        surfaceView = new SurfaceView(getContext());
        setContentView(surfaceView);

        surfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                // Width/height are reported in surfaceChanged.
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                Surface s = holder.getSurface();
                if (s != null && s.isValid()) {
                    Log.i(TAG, "secondary surface " + width + "x" + height);
                    host.provideSecondarySurface(s, width, height);
                }
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                Log.i(TAG, "secondary surface destroyed");
                host.releaseSecondarySurface();
            }
        });

        // Forward touches on the secondary panel to native (ImGui consumes them
        // on the second surface's own context).
        surfaceView.setOnTouchListener((v, ev) -> {
            forwardTouch(ev);
            return true;
        });
    }

    private void forwardTouch(MotionEvent ev) {
        // Map MotionEvent actions to the native scheme (0=down,1=up,2=cancel,3=move).
        final int masked = ev.getActionMasked();
        final int nativeAction;
        switch (masked) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                nativeAction = 0;
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                nativeAction = 1;
                break;
            case MotionEvent.ACTION_CANCEL:
                nativeAction = 2;
                break;
            default:
                nativeAction = 3;  // move
                break;
        }
        final int idx = (masked == MotionEvent.ACTION_POINTER_DOWN
                      || masked == MotionEvent.ACTION_POINTER_UP)
                      ? ev.getActionIndex() : 0;
        final int pointerId = ev.getPointerId(idx);
        host.forwardSecondaryTouch(pointerId, nativeAction, ev.getX(idx), ev.getY(idx));
    }
}
