package com.sunjaycy.goldeneye;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.util.SparseIntArray;
import android.view.Choreographer;
import android.view.MotionEvent;
import android.view.View;

/**
 * On-screen virtual gamepad for ordinary Android phones/tablets (no controller).
 *
 * A translucent, non-focusable overlay the Activity adds over the game whenever
 * no physical controller is present. It draws a left move stick, a right-side
 * look area (swipe or floating stick), Fire/Aim triggers, A/B face buttons, a
 * pause button and a weapons button, tracks full multitouch, and once per frame
 * (Choreographer) pushes a synthesized Xbox 360 pad frame to native via
 * {@link GoldenEyeActivity#forwardTouchState}. Native ORs it into the guest pad
 * (see ge_touchpad.h / ge_hooks.cpp).
 *
 * Nothing here reaches into the game render/present path: drawing is a plain
 * Canvas on a separate panel window, so the low-latency guest-thread present
 * path is untouched.
 *
 * The Weapons button opens an in-overlay grid of the carried weapons (read from
 * the gamestate bridge over JNI); tapping one posts an equip request. Drawing it
 * here (rather than a native ImGui dialog on the primary drawer) keeps it off
 * the present path too.
 */
final class TouchControlsView extends View implements Choreographer.FrameCallback {
    // Xbox 360 button bits (match the guest masks in ge_hooks.cpp).
    private static final int BTN_A = 0x1000;
    private static final int BTN_B = 0x2000;
    private static final int BTN_X = 0x4000;
    private static final int BTN_Y = 0x8000;
    private static final int BTN_START = 0x0010;

    // Pointer roles.
    private static final int ROLE_NONE = 0;
    private static final int ROLE_MOVE = 1;
    private static final int ROLE_LOOK = 2;
    private static final int ROLE_FIRE = 3;
    private static final int ROLE_AIM = 4;
    private static final int ROLE_A = 5;
    private static final int ROLE_B = 6;
    private static final int ROLE_X = 7;
    private static final int ROLE_Y = 8;
    private static final int ROLE_START = 9;
    private static final int ROLE_WEAPONS = 10;

    private final GoldenEyeActivity host;
    private final float density;

    // Config (read once when shown; cheap to refresh).
    private boolean lookStickMode;   // false = swipe, true = floating stick
    private float lookSens = 1.0f;
    private float opacity = 0.5f;

    // Paints.
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);

    // Layout (computed in onSizeChanged).
    private float moveCx, moveCy, moveR;
    private float fireCx, fireCy, fireR;
    private float aimCx, aimCy, aimR;
    // A/B/X/Y face-button diamond (Xbox layout: Y top, X left, B right, A bottom).
    private float aCx, aCy, bCx, bCy, xCx, xCy, yCx, yCy, faceR;
    private float startCx, startCy, startR;
    private float wpnCx, wpnCy, wpnR;

    // Pointer tracking. One MOVE + one LOOK pointer at a time; buttons keyed by id.
    private final SparseIntArray pointerRole = new SparseIntArray();
    private int moveId = -1, lookId = -1;
    private int fireId = -1, aimId = -1, aId = -1, bId = -1, xId = -1, yId = -1, startId = -1;
    private float moveX, moveY;
    private float lookOriginX, lookOriginY, lookCurX, lookCurY, lookLastFrameX, lookLastFrameY;

    // Weapon grid state.
    private boolean gridOpen;
    private final int[] gridIds = new int[32];
    private final int[] gridAmmo = new int[32];
    private int gridCount;
    private int gridEquipped = -1;

    private boolean frameScheduled;

    TouchControlsView(Context context, GoldenEyeActivity host) {
        super(context);
        this.host = host;
        this.density = getResources().getDisplayMetrics().density;
        setBackgroundColor(Color.TRANSPARENT);
        text.setColor(Color.WHITE);
        text.setTextAlign(Paint.Align.CENTER);
        stroke.setStyle(Paint.Style.STROKE);
        fill.setStyle(Paint.Style.FILL);
    }

    private float dp(float v) { return v * density; }

    /** Re-read tuning cvars from native (call when the overlay is shown). */
    void refreshConfig() {
        lookStickMode = "stick".equalsIgnoreCase(host.touchLookMode());
        lookSens = host.touchLookSens();
        if (lookSens <= 0f) lookSens = 1.0f;
        opacity = host.touchOpacity();
        if (opacity < 0.1f) opacity = 0.1f;
        if (opacity > 1f) opacity = 1f;
        invalidate();
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        text.setTextSize(dp(15));
        // Left move stick, bottom-left.
        moveR = dp(70);
        moveCx = dp(24) + moveR;
        moveCy = h - dp(24) - moveR;
        // Right-thumb cluster.
        fireR = dp(52);
        fireCx = w - dp(28) - fireR;
        fireCy = h - dp(28) - fireR;
        aimR = dp(40);
        aimCx = fireCx - dp(120);
        aimCy = fireCy - dp(40);
        // A/B/X/Y diamond, up-left of FIRE. Center + spacing sized for thumbs.
        faceR = dp(36);
        float faceCx = w - dp(175);
        float faceCy = h - dp(285);
        float s = dp(62);
        yCx = faceCx;      yCy = faceCy - s;   // Y (top)
        xCx = faceCx - s;  xCy = faceCy;       // X (left)
        bCx = faceCx + s;  bCy = faceCy;       // B (right)
        aCx = faceCx;      aCy = faceCy + s;   // A (bottom)
        // Top-right small buttons.
        startR = dp(26);
        startCx = w - dp(20) - startR;
        startCy = dp(20) + startR;
        wpnR = dp(30);
        wpnCx = startCx - dp(90);
        wpnCy = startCy;
    }

    // --- Drawing -------------------------------------------------------------

    @Override
    protected void onDraw(Canvas c) {
        final int a = Math.round(opacity * 255);
        if (gridOpen) {
            drawWeaponGrid(c);
            return;
        }
        // Left move stick: base + knob at the current offset.
        drawStickBase(c, moveCx, moveCy, moveR, a);
        if (moveId != -1) {
            float dx = moveX - moveCx, dy = moveY - moveCy;
            float len = (float) Math.hypot(dx, dy);
            if (len > moveR) { dx = dx / len * moveR; dy = dy / len * moveR; }
            drawKnob(c, moveCx + dx, moveCy + dy, dp(30), a);
        } else {
            drawKnob(c, moveCx, moveCy, dp(30), a);
        }
        // Look area hint (floating stick mode shows a ghost stick where held).
        if (lookStickMode && lookId != -1) {
            drawStickBase(c, lookOriginX, lookOriginY, dp(70), a);
            drawKnob(c, lookCurX, lookCurY, dp(28), a);
        }
        // Buttons.
        drawButton(c, fireCx, fireCy, fireR, "FIRE", fireId != -1, a);
        drawButton(c, aimCx, aimCy, aimR, "AIM", aimId != -1, a);
        drawButton(c, yCx, yCy, faceR, "Y", yId != -1, a);
        drawButton(c, xCx, xCy, faceR, "X", xId != -1, a);
        drawButton(c, bCx, bCy, faceR, "B", bId != -1, a);
        drawButton(c, aCx, aCy, faceR, "A", aId != -1, a);
        drawButton(c, startCx, startCy, startR, "II", startId != -1, a);
        drawButton(c, wpnCx, wpnCy, wpnR, "WPN", false, a);
    }

    private void drawStickBase(Canvas c, float cx, float cy, float r, int a) {
        fill.setColor(Color.argb(Math.round(a * 0.35f), 0, 0, 0));
        c.drawCircle(cx, cy, r, fill);
        stroke.setStrokeWidth(dp(2));
        stroke.setColor(Color.argb(a, 230, 230, 230));
        c.drawCircle(cx, cy, r, stroke);
    }

    private void drawKnob(Canvas c, float cx, float cy, float r, int a) {
        fill.setColor(Color.argb(Math.round(a * 0.7f), 210, 210, 210));
        c.drawCircle(cx, cy, r, fill);
    }

    private void drawButton(Canvas c, float cx, float cy, float r, String label,
                            boolean pressed, int a) {
        fill.setColor(pressed ? Color.argb(a, 196, 60, 50)
                              : Color.argb(Math.round(a * 0.45f), 20, 20, 20));
        c.drawCircle(cx, cy, r, fill);
        stroke.setStrokeWidth(dp(2));
        stroke.setColor(Color.argb(a, 235, 235, 235));
        c.drawCircle(cx, cy, r, stroke);
        text.setTextSize(dp(15));  // grid drawing mutates the shared paint size
        text.setColor(Color.argb(Math.max(a, 200), 255, 255, 255));
        c.drawText(label, cx, cy + text.getTextSize() * 0.35f, text);
    }

    private void drawWeaponGrid(Canvas c) {
        final int w = getWidth(), h = getHeight();
        fill.setColor(Color.argb(215, 12, 11, 8));
        c.drawRect(0, 0, w, h, fill);
        text.setTextSize(dp(20));
        text.setColor(Color.WHITE);
        c.drawText("WEAPONS  (tap to switch, tap outside to close)", w / 2f, dp(50), text);
        if (gridCount <= 0) {
            c.drawText("(no weapons)", w / 2f, h / 2f, text);
            return;
        }
        // Responsive grid of large cells.
        int cols = Math.min(4, Math.max(1, (int) (w / dp(240))));
        int rows = (gridCount + cols - 1) / cols;
        float cellW = (w - dp(40)) / cols;
        float cellH = Math.min(dp(96), (h - dp(90)) / Math.max(rows, 1));
        float gx = dp(20), gy = dp(72);
        text.setTextSize(dp(18));
        for (int i = 0; i < gridCount; i++) {
            int col = i % cols, row = i / cols;
            float x0 = gx + col * cellW, y0 = gy + row * cellH;
            float x1 = x0 + cellW - dp(10), y1 = y0 + cellH - dp(10);
            boolean equipped = gridIds[i] == gridEquipped;
            fill.setColor(equipped ? Color.argb(255, 196, 36, 28)
                                   : Color.argb(255, 52, 46, 34));
            c.drawRect(x0, y0, x1, y1, fill);
            text.setColor(Color.WHITE);
            String name = weaponLabel(gridIds[i]);
            c.drawText(name, (x0 + x1) / 2f, (y0 + y1) / 2f, text);
            c.drawText("x" + gridAmmo[i], (x0 + x1) / 2f, (y0 + y1) / 2f + dp(24), text);
        }
    }

    // --- Touch ---------------------------------------------------------------

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        final int action = e.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int idx = e.getActionIndex();
                onPointerDown(e.getPointerId(idx), e.getX(idx), e.getY(idx));
                break;
            }
            case MotionEvent.ACTION_MOVE: {
                for (int i = 0; i < e.getPointerCount(); i++) {
                    onPointerMove(e.getPointerId(i), e.getX(i), e.getY(i));
                }
                break;
            }
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_UP: {
                int idx = e.getActionIndex();
                releasePointer(e.getPointerId(idx));
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                clearAllPointers();
                break;
            default:
                break;
        }
        invalidate();
        return true;
    }

    private boolean hit(float x, float y, float cx, float cy, float r) {
        float slop = dp(8);
        return Math.hypot(x - cx, y - cy) <= r + slop;
    }

    private void onPointerDown(int id, float x, float y) {
        if (gridOpen) {
            onGridTap(x, y);
            return;
        }
        // Buttons take priority over the sticks/look wherever they land.
        int role;
        if (hit(x, y, wpnCx, wpnCy, wpnR)) {
            toggleGrid();
            return;
        } else if (hit(x, y, fireCx, fireCy, fireR)) { role = ROLE_FIRE; fireId = id; }
        else if (hit(x, y, aimCx, aimCy, aimR)) { role = ROLE_AIM; aimId = id; }
        else if (hit(x, y, aCx, aCy, faceR)) { role = ROLE_A; aId = id; }
        else if (hit(x, y, bCx, bCy, faceR)) { role = ROLE_B; bId = id; }
        else if (hit(x, y, xCx, xCy, faceR)) { role = ROLE_X; xId = id; }
        else if (hit(x, y, yCx, yCy, faceR)) { role = ROLE_Y; yId = id; }
        else if (hit(x, y, startCx, startCy, startR)) { role = ROLE_START; startId = id; }
        else if (x < getWidth() / 2f) {
            role = ROLE_MOVE; moveId = id; moveX = x; moveY = y;
        } else if (lookId == -1) {
            role = ROLE_LOOK; lookId = id;
            lookOriginX = lookCurX = lookLastFrameX = x;
            lookOriginY = lookCurY = lookLastFrameY = y;
        } else {
            role = ROLE_NONE;
        }
        if (role != ROLE_NONE) pointerRole.put(id, role);
    }

    private void onPointerMove(int id, float x, float y) {
        switch (pointerRole.get(id, ROLE_NONE)) {
            case ROLE_MOVE: moveX = x; moveY = y; break;
            case ROLE_LOOK: lookCurX = x; lookCurY = y; break;
            default: break;  // buttons: position doesn't matter while held
        }
    }

    private void releasePointer(int id) {
        int role = pointerRole.get(id, ROLE_NONE);
        pointerRole.delete(id);
        switch (role) {
            case ROLE_MOVE: if (moveId == id) moveId = -1; break;
            case ROLE_LOOK: if (lookId == id) lookId = -1; break;
            case ROLE_FIRE: if (fireId == id) fireId = -1; break;
            case ROLE_AIM: if (aimId == id) aimId = -1; break;
            case ROLE_A: if (aId == id) aId = -1; break;
            case ROLE_B: if (bId == id) bId = -1; break;
            case ROLE_X: if (xId == id) xId = -1; break;
            case ROLE_Y: if (yId == id) yId = -1; break;
            case ROLE_START: if (startId == id) startId = -1; break;
            default: break;
        }
    }

    private void clearAllPointers() {
        pointerRole.clear();
        moveId = lookId = fireId = aimId = aId = bId = xId = yId = startId = -1;
    }

    // --- Weapon grid ---------------------------------------------------------

    private void toggleGrid() {
        gridOpen = !gridOpen;
        if (gridOpen) {
            clearAllPointers();          // drop movement so nothing sticks while browsing
            gridEquipped = host.equippedWeaponId();
            int n = host.carriedWeapons(gridIds, gridAmmo);
            gridCount = Math.max(0, n);
            host.forwardTouchState(0, 0, 0, 0, 0, 0, 0);  // zero the pad
        }
        invalidate();
    }

    private void onGridTap(float x, float y) {
        // Reproduce drawWeaponGrid's cell geometry to hit-test taps.
        final int w = getWidth(), h = getHeight();
        int cols = Math.min(4, Math.max(1, (int) (w / dp(240))));
        int rows = (gridCount + cols - 1) / cols;
        float cellW = (w - dp(40)) / cols;
        float cellH = Math.min(dp(96), (h - dp(90)) / Math.max(rows, 1));
        float gx = dp(20), gy = dp(72);
        for (int i = 0; i < gridCount; i++) {
            int col = i % cols, row = i / cols;
            float x0 = gx + col * cellW, y0 = gy + row * cellH;
            float x1 = x0 + cellW - dp(10), y1 = y0 + cellH - dp(10);
            if (x >= x0 && x <= x1 && y >= y0 && y <= y1) {
                host.requestEquipWeapon(gridIds[i]);
                gridOpen = false;
                invalidate();
                return;
            }
        }
        // Tap outside any cell closes the grid.
        gridOpen = false;
        invalidate();
    }

    // --- Per-frame state push (Choreographer) --------------------------------

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        refreshConfig();
        scheduleFrame();
    }

    @Override
    protected void onDetachedFromWindow() {
        Choreographer.getInstance().removeFrameCallback(this);
        frameScheduled = false;
        host.forwardTouchState(0, 0, 0, 0, 0, 0, 0);  // release everything on hide
        super.onDetachedFromWindow();
    }

    private void scheduleFrame() {
        if (!frameScheduled) {
            frameScheduled = true;
            Choreographer.getInstance().postFrameCallback(this);
        }
    }

    @Override
    public void doFrame(long frameTimeNanos) {
        frameScheduled = false;
        pushState();
        if (isAttachedToWindow()) scheduleFrame();
    }

    private void pushState() {
        if (gridOpen) {
            host.forwardTouchState(0, 0, 0, 0, 0, 0, 0);
            return;
        }
        int lx = 0, ly = 0, rx = 0, ry = 0;
        if (moveId != -1) {
            float dx = clampAxis((moveX - moveCx) / moveR);
            float dy = clampAxis((moveY - moveCy) / moveR);
            lx = (int) (dx * 32767);
            ly = (int) (-dy * 32767);   // screen down is -Y on the stick
        }
        if (lookId != -1) {
            if (lookStickMode) {
                float r = dp(90);
                rx = (int) (clampAxis((lookCurX - lookOriginX) / r) * 32767 * lookSens);
                ry = (int) (-clampAxis((lookCurY - lookOriginY) / r) * 32767 * lookSens);
                rx = clampInt(rx);
                ry = clampInt(ry);
            } else {
                // Swipe: per-frame drag velocity -> right-stick deflection. No
                // movement this frame => 0 => the camera stops turning.
                float full = dp(8);  // px/frame for full deflection at sens 1
                rx = clampInt((int) ((lookCurX - lookLastFrameX) / full * 32767 * lookSens));
                ry = clampInt((int) (-(lookCurY - lookLastFrameY) / full * 32767 * lookSens));
                lookLastFrameX = lookCurX;
                lookLastFrameY = lookCurY;
            }
        }
        int buttons = 0;
        if (aId != -1) buttons |= BTN_A;
        if (bId != -1) buttons |= BTN_B;
        if (xId != -1) buttons |= BTN_X;
        if (yId != -1) buttons |= BTN_Y;
        if (startId != -1) buttons |= BTN_START;
        int lt = (aimId != -1) ? 255 : 0;
        int rt = (fireId != -1) ? 255 : 0;
        host.forwardTouchState(buttons, lt, rt, lx, ly, rx, ry);
    }

    private static float clampAxis(float v) {
        return v < -1f ? -1f : (v > 1f ? 1f : v);
    }
    private static int clampInt(int v) {
        return v < -32767 ? -32767 : (v > 32767 ? 32767 : v);
    }

    // Mirror of ge_weaponmenu.cpp WeaponLabel (ids confirmed on-device).
    private static String weaponLabel(int id) {
        switch (id) {
            case 1:  return "Unarmed";
            case 4:  return "PP7 (Uns.)";
            case 5:  return "PP7";
            case 6:  return "DD44";
            case 7:  return "Klobb";
            case 8:  return "KF7 Soviet";
            case 10: return "D5K";
            case 11: return "D5K (Sil.)";
            case 12: return "Phantom";
            case 17: return "Sniper Rifle";
            case 24: return "Grenade Lchr";
            case 26: return "Hand Grenade";
            case 27: return "Timed Mine";
            case 29: return "Remote Mine";
            case 30: return "Detonator";
            default: return "Weapon " + id;
        }
    }
}
