package com.tab5adb.agent;

import android.content.Context;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;

import java.lang.reflect.Method;

/**
 * Input injection over hidden Android APIs — the scrcpy technique.
 *
 * <p>app_process runs with shell uid, which holds INJECT_EVENTS, so
 * {@code InputManager.injectInputEvent()} works with no permission dialog. This
 * is the agent's shared input foundation: keys today (the overlay power / volume
 * / nav buttons) and motion (touch passthrough) / text (keyboard) later all ride
 * the same {@code injectInputEvent} path (protocol.md §4.7 INPUT channel).
 *
 * <p>The framework {@code InputManager} is reached the version-robust way scrcpy
 * settled on: a synthetic system {@link Context} (built off an
 * {@code ActivityThread}) whose {@code getSystemService(INPUT_SERVICE)} returns
 * the real manager — its {@code injectInputEvent(InputEvent, int)} hidden method
 * is stable across Android 10..15, unlike the removed
 * {@code InputManager.getInstance()} / the unstable {@code IInputManager} AIDL.
 */
final class Input {
    private static final int INJECT_MODE_ASYNC = 0;  // InputManager.INJECT_INPUT_EVENT_MODE_ASYNC

    private final Object inputManager;      // android.hardware.input.InputManager
    private final Method injectInputEvent;  // injectInputEvent(InputEvent, int) -> boolean

    private Input(Object inputManager, Method injectInputEvent) {
        this.inputManager = inputManager;
        this.injectInputEvent = injectInputEvent;
    }

    /** Build the injector; throws if the hidden APIs are unreachable. */
    static Input create() throws Exception {
        Context ctx = SystemContext.get();
        Object im = ctx.getSystemService(Context.INPUT_SERVICE);
        if (im == null) throw new IllegalStateException("no INPUT_SERVICE");
        Method m = im.getClass().getMethod("injectInputEvent", InputEvent.class, int.class);
        return new Input(im, m);
    }

    /**
     * Inject one key event on display 0 (the mirrored display). `action` is
     * {@code KeyEvent.ACTION_DOWN}/{@code ACTION_UP}; the event is sourced as a
     * virtual keyboard, matching scrcpy's {@code Device.injectKeyEvent}.
     */
    boolean injectKey(int action, int keyCode, int repeat, int metaState) {
        long now = SystemClock.uptimeMillis();
        KeyEvent ev = new KeyEvent(now, now, action, keyCode, repeat, metaState,
                KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, InputDevice.SOURCE_KEYBOARD);
        return inject(ev);
    }

    // --- touch passthrough (§4.7) -------------------------------------------
    //
    // The Tab5 sends per-pointer DOWN/MOVE/UP (input_type=TOUCH); this keeps the
    // active-pointer set and assembles the composite multi-touch MotionEvent the
    // way scrcpy's PointersState does (the first finger is ACTION_DOWN, later ones
    // ACTION_POINTER_DOWN with the pointer index in the high bits, etc.). The
    // coordinates are already the source's LOGICAL display coords (the Server
    // inverted the mirror geometry via Projection). All calls come from one
    // connection's reader thread, but synchronize defensively.
    private static final int MAX_POINTERS = 10;
    private static final int ACTION_POINTER_INDEX_SHIFT = 8;  // MotionEvent
    private final int[] ptrIds = new int[MAX_POINTERS];
    private final float[] ptrX = new float[MAX_POINTERS];
    private final float[] ptrY = new float[MAX_POINTERS];
    private int ptrCount = 0;
    private long touchDownTime = 0;  // time of the gesture's first ACTION_DOWN

    private int indexOfPointer(int id) {
        for (int i = 0; i < ptrCount; i++) if (ptrIds[i] == id) return i;
        return -1;
    }

    private void removePointer(int index) {
        for (int i = index; i < ptrCount - 1; i++) {
            ptrIds[i] = ptrIds[i + 1];
            ptrX[i] = ptrX[i + 1];
            ptrY[i] = ptrY[i + 1];
        }
        ptrCount--;
    }

    /**
     * Inject one per-pointer touch transition (§4.7). {@code action} is
     * 0=DOWN / 1=MOVE / 2=UP; {@code x,y} are the source's logical display coords
     * (ignored for UP, which releases at the pointer's last position). Returns
     * false if the transition can't be applied (unknown pointer, table full).
     */
    synchronized boolean injectTouch(int action, int pointerId, int x, int y) {
        long now = SystemClock.uptimeMillis();
        int index = indexOfPointer(pointerId);
        switch (action) {
            case 0: {  // DOWN
                if (index >= 0) {  // duplicate down — treat as a move
                    ptrX[index] = x; ptrY[index] = y;
                    return injectMotion(MotionEvent.ACTION_MOVE, now);
                }
                if (ptrCount >= MAX_POINTERS) return false;
                if (ptrCount == 0) touchDownTime = now;
                index = ptrCount++;
                ptrIds[index] = pointerId; ptrX[index] = x; ptrY[index] = y;
                int motion = (ptrCount == 1) ? MotionEvent.ACTION_DOWN
                        : MotionEvent.ACTION_POINTER_DOWN | (index << ACTION_POINTER_INDEX_SHIFT);
                return injectMotion(motion, now);
            }
            case 1: {  // MOVE
                if (index < 0) return false;
                ptrX[index] = x; ptrY[index] = y;
                return injectMotion(MotionEvent.ACTION_MOVE, now);
            }
            default: {  // UP (2) — release at the pointer's last known position
                if (index < 0) return false;
                int motion = (ptrCount == 1) ? MotionEvent.ACTION_UP
                        : MotionEvent.ACTION_POINTER_UP | (index << ACTION_POINTER_INDEX_SHIFT);
                boolean ok = injectMotion(motion, now);
                removePointer(index);
                return ok;
            }
        }
    }

    private boolean injectMotion(int action, long now) {
        MotionEvent.PointerProperties[] props = new MotionEvent.PointerProperties[ptrCount];
        MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[ptrCount];
        for (int i = 0; i < ptrCount; i++) {
            MotionEvent.PointerProperties pp = new MotionEvent.PointerProperties();
            pp.id = ptrIds[i];
            pp.toolType = MotionEvent.TOOL_TYPE_FINGER;
            props[i] = pp;
            MotionEvent.PointerCoords pc = new MotionEvent.PointerCoords();
            pc.x = ptrX[i]; pc.y = ptrY[i]; pc.pressure = 1f; pc.size = 1f;
            coords[i] = pc;
        }
        MotionEvent ev = MotionEvent.obtain(touchDownTime, now, action, ptrCount,
                props, coords, 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
        boolean ok = inject(ev);
        ev.recycle();
        return ok;
    }

    private boolean inject(InputEvent ev) {
        try {
            return (boolean) injectInputEvent.invoke(inputManager, ev, INJECT_MODE_ASYNC);
        } catch (Exception e) {
            System.err.println("tab5adb-agent: injectInputEvent failed: " + e);
            return false;
        }
    }
}
