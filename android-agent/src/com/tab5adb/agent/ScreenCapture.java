package com.tab5adb.agent;

import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.hardware.display.VirtualDisplay;
import android.media.Image;
import android.media.ImageReader;
import android.os.IBinder;
import android.view.Surface;

import java.lang.reflect.Method;
import java.nio.ByteBuffer;

/**
 * Real screen capture for the mirror stream — scrcpy style, no APK. Running under
 * app_process with shell uid, it reaches hidden display APIs to create a virtual
 * display that mirrors the main display's layer stack into an {@link ImageReader}
 * surface, then hands out each frame as a {@link Bitmap} for {@link FramePipeline}
 * to split into strips.
 *
 * <p>Two creation paths, primary then fallback (same order scrcpy uses):
 *
 * <ul>
 *   <li><b>{@code DisplayManager.createVirtualDisplay}</b> (primary) — the hidden
 *       static mirror helper on {@code android.hardware.display.DisplayManager}.
 *       It mirrors display 0 into the panel-sized {@link ImageReader} surface and
 *       the system compositor does the rotate / scale-fit / black-letterbox into
 *       that surface (aspect-preserving mirroring), so {@code acquire()} already
 *       returns the final upright, scaled, letterboxed panel-sized frame. This is
 *       the path that works on Android 14 / 15, where
 *       {@code SurfaceControl.createDisplay} was removed.
 *   <li><b>{@code SurfaceControl.createDisplay} + {@code setDisplayProjection}</b>
 *       (fallback, pre-12 / pre-14 where the static helper is absent) — we create
 *       the virtual display by hand and drive the geometry ourselves via {@link
 *       Projection} (rotation code + a centered destination rect, the rest left as
 *       the display's black background = the letterbox).
 * </ul>
 *
 * <p>Either way the GPU does the geometry — no CPU readback at source resolution
 * and no rotate/scale/composite Bitmap copies; {@link FramePipeline} then only
 * strips + JPEG-encodes. (The {@link TestPattern} path still runs the CPU geometry
 * in {@link FramePipeline}, since the GPU projection needs a real SurfaceFlinger.)
 * Only the fallback honours {@code scaleMode} (fill vs fit) via {@link Projection};
 * the primary mirror path is always aspect-fit, which is the mirror default.
 *
 * <p>All hidden-API calls go through reflection; a total failure (both paths)
 * surfaces as an exception that aborts the connection rather than silently
 * degrading. Use {@code --test-pattern} to exercise the pipeline without touching
 * these APIs.
 */
final class ScreenCapture {
    private final int readerW;  // = target panel width (the projected output size)
    private final int readerH;
    private final ImageReader reader;

    // Exactly one of these is non-null, depending on which creation path took.
    private VirtualDisplay virtualDisplay;  // primary: DisplayManager path
    private Class<?> sc;                     // fallback: android.view.SurfaceControl
    private IBinder display;                 // fallback: the SurfaceControl display token

    ScreenCapture(int srcW, int srcH, int targetW, int targetH, int scaleMode) throws Exception {
        this.readerW = targetW;
        this.readerH = targetH;
        // The reader is the Tab5 panel size; the compositor scales the source into it.
        this.reader = ImageReader.newInstance(targetW, targetH, PixelFormat.RGBA_8888, 3);
        Surface surface = reader.getSurface();

        try {
            this.virtualDisplay = createVirtualDisplay(targetW, targetH, surface);
            if (virtualDisplay == null) {
                throw new IllegalStateException("createVirtualDisplay returned null");
            }
        } catch (Throwable dmError) {
            // Older Android (no static DisplayManager.createVirtualDisplay): fall
            // back to the legacy SurfaceControl projection.
            try {
                createSurfaceControlDisplay(srcW, srcH, targetW, targetH, scaleMode, surface);
            } catch (Throwable scError) {
                reader.close();
                throw new IllegalStateException(
                        "could not create capture display (DisplayManager: " + dmError
                                + "; SurfaceControl: " + scError + ")", scError);
            }
        }
    }

    /**
     * Primary path: the hidden static {@code DisplayManager.createVirtualDisplay(
     * name, width, height, displayIdToMirror, surface)}. Mirrors display 0 into the
     * surface; the system handles rotation / scale-fit / letterbox.
     */
    private static VirtualDisplay createVirtualDisplay(int width, int height, Surface surface) throws Exception {
        Method m = android.hardware.display.DisplayManager.class
                .getMethod("createVirtualDisplay", String.class, int.class, int.class, int.class, Surface.class);
        return (VirtualDisplay) m.invoke(null, "tab5adb-capture", width, height, 0, surface);
    }

    /**
     * Fallback path: create the virtual display via {@code SurfaceControl} and drive
     * the rotate/scale-fit/letterbox geometry ourselves with {@link Projection}.
     */
    private void createSurfaceControlDisplay(int srcW, int srcH, int targetW, int targetH, int scaleMode, Surface surface)
            throws Exception {
        this.sc = Class.forName("android.view.SurfaceControl");

        IBinder disp = (IBinder) sc.getMethod("createDisplay", String.class, boolean.class)
                .invoke(null, "tab5adb-capture", false);
        if (disp == null) {
            throw new IllegalStateException("SurfaceControl.createDisplay returned null");
        }
        this.display = disp;

        // GPU projection: rotate the source upright and scale-fit it into a centered
        // rect inside the panel-sized reader; the area outside the rect is the
        // virtual display's black background = the letterbox (no CPU composite).
        Projection p = Projection.compute(srcW, srcH, targetW, targetH, scaleMode);
        sc.getMethod("openTransaction").invoke(null);
        try {
            sc.getMethod("setDisplaySurface", IBinder.class, Surface.class)
                    .invoke(null, display, surface);
            sc.getMethod("setDisplayProjection", IBinder.class, int.class, Rect.class, Rect.class)
                    .invoke(null, display, p.orientation,
                            new Rect(p.layerStackL, p.layerStackT, p.layerStackR, p.layerStackB),
                            new Rect(p.dispL, p.dispT, p.dispR, p.dispB));
            // Layer stack 0 = the default display's content.
            sc.getMethod("setDisplayLayerStack", IBinder.class, int.class)
                    .invoke(null, display, 0);
        } finally {
            sc.getMethod("closeTransaction").invoke(null);
        }
    }

    /**
     * Latest captured frame as a panel-sized (targetW×targetH) ARGB_8888 bitmap,
     * already rotated / scaled / letterboxed by the GPU, or null if none is ready
     * yet (e.g. a static screen produces no new frames — the caller then sends
     * nothing and the Tab5 keeps showing the last frame).
     */
    Bitmap acquire() {
        Image img = reader.acquireLatestImage();
        if (img == null) return null;
        try {
            Image.Plane plane = img.getPlanes()[0];
            ByteBuffer buf = plane.getBuffer();
            int pixelStride = plane.getPixelStride();
            int rowStride = plane.getRowStride();
            int rowPadding = rowStride - pixelStride * readerW;
            // Account for row padding by widening the bitmap, then crop it away.
            int paddedW = readerW + (pixelStride > 0 ? rowPadding / pixelStride : 0);
            Bitmap padded = Bitmap.createBitmap(paddedW, readerH, Bitmap.Config.ARGB_8888);
            buf.rewind();
            padded.copyPixelsFromBuffer(buf);
            if (rowPadding == 0) return padded;
            Bitmap cropped = Bitmap.createBitmap(padded, 0, 0, readerW, readerH);
            if (cropped != padded) padded.recycle();
            return cropped;
        } finally {
            img.close();
        }
    }

    void close() {
        if (virtualDisplay != null) {
            try {
                virtualDisplay.release();
            } catch (Throwable ignored) {
                // best-effort teardown
            }
        }
        if (sc != null && display != null) {
            try {
                sc.getMethod("destroyDisplay", IBinder.class).invoke(null, display);
            } catch (Throwable ignored) {
                // best-effort teardown
            }
        }
        reader.close();
    }
}
