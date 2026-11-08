/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;

import java.lang.reflect.Method;
import java.util.LinkedHashMap;
import java.util.Map;

// Turns TOUCH and KEY into events injected through IInputManager.
final class Input {
    private static final int INJECT_MODE_ASYNC = 0;
    private static final int MAX_POINTERS = 10;
    private static final long UINT32_MASK = 0xFFFFFFFFL;

    private final Object inputManager;
    private final Method injectInputEvent;
    private final int[] ids = new int[MAX_POINTERS];
    // Android takes pointer ids up to 31 only; the Tab5's run up to 255.
    private final int[] pointerIds = new int[MAX_POINTERS];
    private final float[] xs = new float[MAX_POINTERS];
    private final float[] ys = new float[MAX_POINTERS];
    private int count;
    private long sourceAnchor;
    private long uptimeAnchor;
    private long downTime;
    private long lastTime;
    private final Map<Integer, Long> heldKeys = new LinkedHashMap<>();

    private Input(Object inputManager, Method injectInputEvent) {
        this.inputManager = inputManager;
        this.injectInputEvent = injectInputEvent;
    }

    static Input create() throws Exception {
        Class<?> sm = Class.forName("android.os.ServiceManager");
        Object binder = sm.getMethod("getService", String.class).invoke(null, "input");
        Class<?> stub = Class.forName("android.hardware.input.IInputManager$Stub");
        Object im = stub.getMethod("asInterface", Class.forName("android.os.IBinder")).invoke(null, binder);
        return new Input(im, im.getClass().getMethod("injectInputEvent", InputEvent.class, int.class));
    }

    synchronized void touch(long time, int n, int[] pid, float[] px, float[] py) {
        if (count == 0 && n > 0) {
            sourceAnchor = time;
            uptimeAnchor = SystemClock.uptimeMillis();
            lastTime = 0;
        }
        long when = eventTime(time);
        boolean moved = false;
        for (int i = 0; i < count; i++) {
            int at = indexOf(pid, n, ids[i]);
            if (at >= 0 && (xs[i] != px[at] || ys[i] != py[at])) {
                xs[i] = px[at];
                ys[i] = py[at];
                moved = true;
            }
        }
        boolean changed = false;
        for (int i = count - 1; i >= 0; i--) {
            if (indexOf(pid, n, ids[i]) >= 0) {
                continue;
            }
            injectMotion(count == 1 ? MotionEvent.ACTION_UP : MotionEvent.ACTION_POINTER_UP | (i << MotionEvent.ACTION_POINTER_INDEX_SHIFT), when);
            remove(i);
            changed = true;
        }
        for (int i = 0; i < n; i++) {
            if (indexOf(ids, count, pid[i]) >= 0 || count >= MAX_POINTERS) {
                continue;
            }
            if (count == 0) {
                downTime = when;
            }
            ids[count] = pid[i];
            pointerIds[count] = freePointerId();
            xs[count] = px[i];
            ys[count] = py[i];
            count++;
            injectMotion(count == 1 ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_POINTER_DOWN | ((count - 1) << MotionEvent.ACTION_POINTER_INDEX_SHIFT), when);
            changed = true;
        }
        if (moved && !changed && count > 0) {
            injectMotion(MotionEvent.ACTION_MOVE, when);
        }
    }

    synchronized void cancel() {
        if (count == 0) {
            return;
        }
        injectMotion(MotionEvent.ACTION_CANCEL, SystemClock.uptimeMillis());
        count = 0;
    }

    synchronized void key(int code, boolean down) {
        long now = SystemClock.uptimeMillis();
        if (down) {
            if (heldKeys.containsKey(code)) {
                return;
            }
            heldKeys.put(code, now);
            injectKey(now, now, KeyEvent.ACTION_DOWN, code, 0);
            return;
        }
        Long downAt = heldKeys.remove(code);
        if (downAt != null) {
            injectKey(downAt, now, KeyEvent.ACTION_UP, code, 0);
        }
    }

    synchronized void clear() {
        cancel();
        long now = SystemClock.uptimeMillis();
        for (Map.Entry<Integer, Long> held : heldKeys.entrySet()) {
            injectKey(held.getValue(), now, KeyEvent.ACTION_UP, held.getKey(), KeyEvent.FLAG_CANCELED);
        }
        heldKeys.clear();
    }

    private long eventTime(long time) {
        long delta = (time - sourceAnchor) & UINT32_MASK;
        if (delta >= 0x80000000L) {
            delta -= 0x100000000L;
        }
        long when = Math.min(uptimeAnchor + delta, SystemClock.uptimeMillis());
        when = Math.max(when, lastTime);
        lastTime = when;
        return when;
    }

    private int freePointerId() {
        int id = 0;
        while (indexOf(pointerIds, count, id) >= 0) {
            id++;
        }
        return id;
    }

    private static int indexOf(int[] of, int n, int id) {
        for (int i = 0; i < n; i++) {
            if (of[i] == id) {
                return i;
            }
        }
        return -1;
    }

    private void remove(int index) {
        for (int i = index; i < count - 1; i++) {
            ids[i] = ids[i + 1];
            pointerIds[i] = pointerIds[i + 1];
            xs[i] = xs[i + 1];
            ys[i] = ys[i + 1];
        }
        count--;
    }

    private void injectKey(long downAt, long when, int action, int code, int flags) {
        inject(new KeyEvent(downAt, when, action, code, 0, 0, KeyCharacterMap.VIRTUAL_KEYBOARD, 0, flags, InputDevice.SOURCE_KEYBOARD), "key " + code + " action " + action);
    }

    private void injectMotion(int action, long when) {
        MotionEvent.PointerProperties[] props = new MotionEvent.PointerProperties[count];
        MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[count];
        for (int i = 0; i < count; i++) {
            props[i] = new MotionEvent.PointerProperties();
            props[i].id = pointerIds[i];
            props[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coords[i] = new MotionEvent.PointerCoords();
            coords[i].x = xs[i];
            coords[i].y = ys[i];
            coords[i].pressure = 1f;
            coords[i].size = 1f;
        }
        MotionEvent event = MotionEvent.obtain(downTime, when, action, count, props, coords, 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
        inject(event, "action " + action + " pointers " + count);
        event.recycle();
    }

    private void inject(InputEvent event, String what) {
        try {
            if (!(boolean) injectInputEvent.invoke(inputManager, event, INJECT_MODE_ASYNC)) {
                System.out.println("inject: refused " + what);
            }
        } catch (Exception e) {
            System.out.println("inject: " + e);
        }
    }
}
