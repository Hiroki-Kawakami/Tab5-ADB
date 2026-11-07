/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import java.lang.reflect.Method;

// Holds display 0 at a rotation through IWindowManager, and puts back what the
// user had before the first hold.
final class RotationLock {
    private static final String CALLER = "tab5adb-agent";

    private static boolean held;
    private static boolean wasFrozen;
    private static int wasRotation;

    private RotationLock() {
    }

    static synchronized void hold(int rotation) throws Exception {
        Object wm = windowManager();
        if (!held) {
            wasFrozen = isFrozen(wm);
            wasRotation = ScreenCapture.rotation();
        }
        freeze(wm, rotation);
        held = true;
    }

    static synchronized void release() {
        if (!held) {
            return;
        }
        held = false;
        try {
            Object wm = windowManager();
            if (wasFrozen) {
                freeze(wm, wasRotation);
            } else {
                thaw(wm);
            }
        } catch (Throwable t) {
            System.out.println("rotation release: " + t);
        }
    }

    private static Object windowManager() throws Exception {
        Class<?> sm = Class.forName("android.os.ServiceManager");
        Object binder = sm.getMethod("getService", String.class).invoke(null, "window");
        Class<?> stub = Class.forName("android.view.IWindowManager$Stub");
        return stub.getMethod("asInterface", Class.forName("android.os.IBinder")).invoke(null, binder);
    }

    private static Method find(Object wm, String name, Class<?>... params) {
        try {
            return wm.getClass().getMethod(name, params);
        } catch (NoSuchMethodException e) {
            return null;
        }
    }

    // Which of these exist depends on the Android version.
    private static void freeze(Object wm, int rotation) throws Exception {
        Method m = find(wm, "freezeDisplayRotation", int.class, int.class, String.class);
        if (m != null) {
            m.invoke(wm, 0, rotation, CALLER);
            return;
        }
        m = find(wm, "freezeDisplayRotation", int.class, int.class);
        if (m != null) {
            m.invoke(wm, 0, rotation);
            return;
        }
        wm.getClass().getMethod("freezeRotation", int.class).invoke(wm, rotation);
    }

    private static void thaw(Object wm) throws Exception {
        Method m = find(wm, "thawDisplayRotation", int.class, String.class);
        if (m != null) {
            m.invoke(wm, 0, CALLER);
            return;
        }
        m = find(wm, "thawDisplayRotation", int.class);
        if (m != null) {
            m.invoke(wm, 0);
            return;
        }
        wm.getClass().getMethod("thawRotation").invoke(wm);
    }

    private static boolean isFrozen(Object wm) throws Exception {
        Method m = find(wm, "isDisplayRotationFrozen", int.class);
        if (m != null) {
            return (boolean) m.invoke(wm, 0);
        }
        return (boolean) wm.getClass().getMethod("isRotationFrozen").invoke(wm);
    }
}
