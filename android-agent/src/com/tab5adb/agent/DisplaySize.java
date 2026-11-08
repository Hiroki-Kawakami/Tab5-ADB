/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import android.graphics.Point;

// Forces display 0 to an aspect through IWindowManager, and clears it again.
final class DisplaySize {
    private static boolean held;

    private DisplaySize() {
    }

    // false when the user has forced a size already, which is left alone.
    static synchronized boolean adapt(int width, int height) throws Exception {
        Object wm = RotationLock.windowManager();
        Point initial = size(wm, "getInitialDisplaySize");
        if (!held && !size(wm, "getBaseDisplaySize").equals(initial)) {
            return false;
        }
        int shortEdge = Math.min(initial.x, initial.y);
        int longEdge = (int) Math.round((double) shortEdge * Math.max(width, height) / Math.min(width, height)) & ~1;
        Point target = initial.x <= initial.y ? new Point(shortEdge, longEdge) : new Point(longEdge, shortEdge);
        if (target.equals(initial)) {
            release();
            return true;
        }
        wm.getClass().getMethod("setForcedDisplaySize", int.class, int.class, int.class).invoke(wm, 0, target.x, target.y);
        held = true;
        System.out.println("display: " + target.x + "x" + target.y);
        return true;
    }

    static synchronized void release() {
        if (!held) {
            return;
        }
        held = false;
        try {
            Object wm = RotationLock.windowManager();
            wm.getClass().getMethod("clearForcedDisplaySize", int.class).invoke(wm, 0);
        } catch (Throwable t) {
            System.out.println("display release: " + t);
        }
    }

    private static Point size(Object wm, String method) throws Exception {
        Point size = new Point();
        wm.getClass().getMethod(method, int.class, Point.class).invoke(wm, 0, size);
        return size;
    }
}
