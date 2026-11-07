/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.graphics.Point;
import android.graphics.Rect;
import android.hardware.display.VirtualDisplay;
import android.media.Image;
import android.media.ImageReader;
import android.os.IBinder;
import android.view.Surface;

import java.lang.reflect.Method;
import java.nio.ByteBuffer;

// Display 0 as it is drawn now, scaled by the compositor into a reader of
// width x height.
final class ScreenCapture {
    final int width;
    final int height;
    private final ImageReader reader;
    private VirtualDisplay virtualDisplay;
    private IBinder display;

    ScreenCapture(int width, int height, int logicalW, int logicalH) throws Exception {
        this.width = width;
        this.height = height;
        reader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 2);
        try {
            Method m = android.hardware.display.DisplayManager.class
                    .getMethod("createVirtualDisplay", String.class, int.class, int.class, int.class, Surface.class);
            virtualDisplay = (VirtualDisplay) m.invoke(null, "tab5adb-capture", width, height, 0, reader.getSurface());
            if (virtualDisplay == null) {
                throw new IllegalStateException("createVirtualDisplay returned null");
            }
        } catch (Throwable dmError) {
            // Before Android 12 the static mirror helper does not exist.
            try {
                createSurfaceControlDisplay(logicalW, logicalH);
            } catch (Throwable scError) {
                reader.close();
                throw new IllegalStateException("capture display: " + dmError + "; " + scError, scError);
            }
        }
    }

    private void createSurfaceControlDisplay(int logicalW, int logicalH) throws Exception {
        Class<?> sc = Class.forName("android.view.SurfaceControl");
        display = (IBinder) sc.getMethod("createDisplay", String.class, boolean.class).invoke(null, "tab5adb-capture", false);
        if (display == null) {
            throw new IllegalStateException("SurfaceControl.createDisplay returned null");
        }
        sc.getMethod("openTransaction").invoke(null);
        try {
            sc.getMethod("setDisplaySurface", IBinder.class, Surface.class).invoke(null, display, reader.getSurface());
            sc.getMethod("setDisplayProjection", IBinder.class, int.class, Rect.class, Rect.class)
                    .invoke(null, display, 0, new Rect(0, 0, logicalW, logicalH), new Rect(0, 0, width, height));
            sc.getMethod("setDisplayLayerStack", IBinder.class, int.class).invoke(null, display, 0);
        } finally {
            sc.getMethod("closeTransaction").invoke(null);
        }
    }

    // null while nothing new was drawn.
    Bitmap acquire() {
        Image img = reader.acquireLatestImage();
        if (img == null) {
            return null;
        }
        try {
            Image.Plane plane = img.getPlanes()[0];
            ByteBuffer buf = plane.getBuffer();
            int pixelStride = plane.getPixelStride();
            int paddedW = plane.getRowStride() / pixelStride;
            Bitmap padded = Bitmap.createBitmap(paddedW, height, Bitmap.Config.ARGB_8888);
            buf.rewind();
            padded.copyPixelsFromBuffer(buf);
            if (paddedW == width) {
                return padded;
            }
            Bitmap frame = Bitmap.createBitmap(padded, 0, 0, width, height);
            padded.recycle();
            return frame;
        } finally {
            img.close();
        }
    }

    void close() {
        if (virtualDisplay != null) {
            try {
                virtualDisplay.release();
            } catch (Throwable ignored) {
            }
        }
        if (display != null) {
            try {
                Class.forName("android.view.SurfaceControl").getMethod("destroyDisplay", IBinder.class).invoke(null, display);
            } catch (Throwable ignored) {
            }
        }
        reader.close();
    }

    static Object realDisplay() throws Exception {
        Class<?> dmg = Class.forName("android.hardware.display.DisplayManagerGlobal");
        Object inst = dmg.getMethod("getInstance").invoke(null);
        return dmg.getMethod("getRealDisplay", int.class).invoke(inst, 0);
    }

    // The size of display 0 as it is drawn now.
    static Point logicalSize() throws Exception {
        Object display = realDisplay();
        Point size = new Point();
        display.getClass().getMethod("getRealSize", Point.class).invoke(display, size);
        if (size.x <= 0 || size.y <= 0) {
            throw new IllegalStateException("display size " + size.x + "x" + size.y);
        }
        return size;
    }

    static int rotation() {
        try {
            Object display = realDisplay();
            return (int) display.getClass().getMethod("getRotation").invoke(display) & 3;
        } catch (Throwable t) {
            System.out.println("getRotation: " + t);
            return 0;
        }
    }
}
