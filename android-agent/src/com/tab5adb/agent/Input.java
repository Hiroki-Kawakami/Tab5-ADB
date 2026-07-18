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

    private static final int MAX_POINTERS = 10;
    private static final int ACTION_POINTER_INDEX_SHIFT = 8;
    private static final long UINT32_MASK = 0xFFFFFFFFL;
    private static final long UINT32_HALF = 0x80000000L;
    private static final long UINT32_MOD = 0x100000000L;
    private final int[] ptrIds = new int[MAX_POINTERS];
    private final float[] ptrX = new float[MAX_POINTERS];
    private final float[] ptrY = new float[MAX_POINTERS];
    private int ptrCount = 0;
    private long frameSourceAnchor = 0;
    private long frameUptimeAnchor = 0;
    private long gestureSourceAnchor = 0;
    private long gestureUptimeAnchor = 0;
    private long touchDownTime = 0;
    private long lastEventTime = 0;

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

    private static int indexOfSnapshotPointer(int id, int count, int[] ids) {
        for (int i = 0; i < count; ++i) if (ids[i] == id) return i;
        return -1;
    }

    synchronized void beginTouchFrame(long newestSourceTime) {
        frameSourceAnchor = newestSourceTime & UINT32_MASK;
        frameUptimeAnchor = SystemClock.uptimeMillis();
    }

    private static long sourceDelta(long value, long base) {
        long delta = (value - base) & UINT32_MASK;
        return delta >= UINT32_HALF ? delta - UINT32_MOD : delta;
    }

    private void beginGesture(long sampleTime) {
        gestureSourceAnchor = frameSourceAnchor;
        gestureUptimeAnchor = frameUptimeAnchor;
        lastEventTime = 0;
        touchDownTime = touchEventTime(sampleTime);
    }

    private long touchEventTime(long sampleTime) {
        long eventTime = gestureUptimeAnchor +
                sourceDelta(sampleTime & UINT32_MASK, gestureSourceAnchor);
        long now = SystemClock.uptimeMillis();
        if (eventTime > now) eventTime = now;
        if (lastEventTime != 0 && eventTime < lastEventTime) eventTime = lastEventTime;
        return eventTime;
    }

    synchronized boolean injectTouchSnapshot(long sampleTime, int count,
                                              int[] ids, int[] xs, int[] ys) {
        long eventTime = ptrCount > 0 ? touchEventTime(sampleTime) : 0;
        boolean moved = false;
        for (int i = 0; i < ptrCount; ++i) {
            int source = indexOfSnapshotPointer(ptrIds[i], count, ids);
            if (source < 0 || xs[source] < 0 || ys[source] < 0) continue;
            if (ptrX[i] != xs[source] || ptrY[i] != ys[source]) moved = true;
            ptrX[i] = xs[source];
            ptrY[i] = ys[source];
        }

        boolean changedPointers = false;
        boolean ok = true;
        for (int i = ptrCount - 1; i >= 0; --i) {
            if (indexOfSnapshotPointer(ptrIds[i], count, ids) >= 0) continue;
            int action = ptrCount == 1 ? MotionEvent.ACTION_UP
                    : MotionEvent.ACTION_POINTER_UP | (i << ACTION_POINTER_INDEX_SHIFT);
            ok &= injectMotion(action, eventTime);
            lastEventTime = eventTime;
            removePointer(i);
            changedPointers = true;
        }

        for (int i = 0; i < count; ++i) {
            if (xs[i] < 0 || ys[i] < 0 || indexOfPointer(ids[i]) >= 0) continue;
            if (ptrCount >= MAX_POINTERS) {
                ok = false;
                continue;
            }
            if (ptrCount == 0) {
                beginGesture(sampleTime);
                eventTime = touchEventTime(sampleTime);
            }
            int index = ptrCount++;
            ptrIds[index] = ids[i];
            ptrX[index] = xs[i];
            ptrY[index] = ys[i];
            int action = ptrCount == 1 ? MotionEvent.ACTION_DOWN
                    : MotionEvent.ACTION_POINTER_DOWN |
                    (index << ACTION_POINTER_INDEX_SHIFT);
            ok &= injectMotion(action, eventTime);
            lastEventTime = eventTime;
            changedPointers = true;
        }

        if (moved && !changedPointers && ptrCount > 0) {
            ok &= injectMotion(MotionEvent.ACTION_MOVE, eventTime);
            lastEventTime = eventTime;
        }
        if (ptrCount == 0) {
            touchDownTime = 0;
            lastEventTime = 0;
        }
        return ok;
    }

    synchronized void cancelTouch() {
        if (ptrCount == 0) return;
        long now = SystemClock.uptimeMillis();
        injectMotion(MotionEvent.ACTION_CANCEL, now);
        ptrCount = 0;
        touchDownTime = 0;
        lastEventTime = 0;
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
