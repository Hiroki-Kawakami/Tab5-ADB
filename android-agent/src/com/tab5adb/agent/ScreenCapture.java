package com.tab5adb.agent;

import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.media.Image;
import android.media.ImageReader;
import android.os.IBinder;
import android.view.Surface;

import java.nio.ByteBuffer;

/**
 * Real screen capture for the mirror stream — scrcpy style, no APK. Running under
 * app_process with shell uid, it reaches the hidden {@code SurfaceControl} display
 * APIs to create a virtual display that mirrors the main display's layer stack
 * into an {@link ImageReader} surface, then hands out each frame as a {@link
 * Bitmap} for {@link FramePipeline} to rotate / scale / strip.
 *
 * <p>Capture is at the source's physical resolution; all geometry (rotate / scale
 * / split) happens in {@link FramePipeline}, so it is identical to the {@link
 * TestPattern} path and verified the same way.
 *
 * <p>All {@code SurfaceControl} calls go through reflection (the class is hidden);
 * a failure here surfaces as an exception that aborts the connection rather than
 * silently degrading. Use {@code --test-pattern} to exercise the pipeline without
 * touching these APIs.
 */
final class ScreenCapture {
    private final int srcW;
    private final int srcH;
    private final Class<?> sc;  // android.view.SurfaceControl
    private final ImageReader reader;
    private final IBinder display;

    ScreenCapture(int srcW, int srcH) throws Exception {
        this.srcW = srcW;
        this.srcH = srcH;
        this.sc = Class.forName("android.view.SurfaceControl");
        this.reader = ImageReader.newInstance(srcW, srcH, PixelFormat.RGBA_8888, 3);

        IBinder disp = (IBinder) sc.getMethod("createDisplay", String.class, boolean.class)
                .invoke(null, "tab5adb-capture", false);
        if (disp == null) {
            reader.close();
            throw new IllegalStateException("SurfaceControl.createDisplay returned null");
        }
        this.display = disp;

        Surface surface = reader.getSurface();
        sc.getMethod("openTransaction").invoke(null);
        try {
            sc.getMethod("setDisplaySurface", IBinder.class, Surface.class)
                    .invoke(null, display, surface);
            // Mirror the full source into the full reader (no rotate/scale here).
            sc.getMethod("setDisplayProjection", IBinder.class, int.class, Rect.class, Rect.class)
                    .invoke(null, display, 0, new Rect(0, 0, srcW, srcH), new Rect(0, 0, srcW, srcH));
            // Layer stack 0 = the default display's content.
            sc.getMethod("setDisplayLayerStack", IBinder.class, int.class)
                    .invoke(null, display, 0);
        } finally {
            sc.getMethod("closeTransaction").invoke(null);
        }
    }

    /** Latest captured frame as an ARGB_8888 bitmap, or null if none is ready yet. */
    Bitmap acquire() {
        Image img = reader.acquireLatestImage();
        if (img == null) return null;
        try {
            Image.Plane plane = img.getPlanes()[0];
            ByteBuffer buf = plane.getBuffer();
            int pixelStride = plane.getPixelStride();
            int rowStride = plane.getRowStride();
            int rowPadding = rowStride - pixelStride * srcW;
            // Account for row padding by widening the bitmap, then crop it away.
            int paddedW = srcW + (pixelStride > 0 ? rowPadding / pixelStride : 0);
            Bitmap padded = Bitmap.createBitmap(paddedW, srcH, Bitmap.Config.ARGB_8888);
            buf.rewind();
            padded.copyPixelsFromBuffer(buf);
            if (rowPadding == 0) return padded;
            Bitmap cropped = Bitmap.createBitmap(padded, 0, 0, srcW, srcH);
            if (cropped != padded) padded.recycle();
            return cropped;
        } finally {
            img.close();
        }
    }

    void close() {
        try {
            if (display != null) {
                sc.getMethod("destroyDisplay", IBinder.class).invoke(null, display);
            }
        } catch (Throwable ignored) {
            // best-effort teardown
        }
        reader.close();
    }
}
