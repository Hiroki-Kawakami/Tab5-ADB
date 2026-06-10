package com.tab5adb.agent;

import android.graphics.Bitmap;
import android.graphics.Matrix;
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
 *       It mirrors display 0 into the reader surface and the system compositor does
 *       the scale-fit / black-letterbox into that surface (aspect-preserving
 *       mirroring). This is the path that works on Android 14 / 15, where
 *       {@code SurfaceControl.createDisplay} was removed.
 *   <li><b>{@code SurfaceControl.createDisplay} + {@code setDisplayProjection}</b>
 *       (fallback, pre-12 / pre-14 where the static helper is absent) — we create
 *       the virtual display by hand and drive the geometry ourselves via {@link
 *       Projection} (rotation code + a centered destination rect, the rest left as
 *       the display's black background = the letterbox).
 * </ul>
 *
 * <p>Either way the GPU does the scaling — no CPU readback at source resolution and
 * no scale/composite Bitmap copies; {@link FramePipeline} then only strips +
 * JPEG-encodes. (The {@link TestPattern} path still runs the CPU geometry in {@link
 * FramePipeline}, since the GPU projection needs a real SurfaceFlinger.)
 *
 * <h2>Physical-orientation lock (protocol.md §5.1)</h2>
 *
 * <p>The Tab5 is fixed to the device's <em>physical</em> (natural) orientation —
 * logical screen rotation is not considered. But the mirror reflects display 0's
 * <em>current logical rotation</em>: turn the phone and the mirror would rotate and
 * shrink-letterbox with it. So this capture is built for the device's current
 * {@code rotation} (a {@code Surface.ROTATION_*} code; the caller recreates the
 * capture when it changes) and undoes it:
 *
 * <ol>
 *   <li>The reader is sized to the panel <em>oriented to match the rotation</em>
 *       (portrait {@code targetW×targetH} at ROTATION_0/180, landscape
 *       {@code targetH×targetW} at ROTATION_90/270), so the rotated logical display
 *       fills the reader (the compositor still does the scale-fit on the GPU; no
 *       letterbox from an orientation mismatch).
 *   <li>{@link #acquire} counter-rotates that frame by the inverse of the device
 *       rotation, yielding the natural-orientation {@code targetW×targetH} panel
 *       frame — i.e. exactly the device's physical framebuffer. A landscape app then
 *       appears sideways and full-size on the Tab5 (turn the Tab5 to view it), never
 *       rotated-upright-and-letterboxed.
 * </ol>
 *
 * <p>At ROTATION_0 (the common case) the reader is panel-sized and the counter-
 * rotation is a no-op, so it is the same GPU-only fast path as before. Only a rotated
 * device pays one panel-sized {@link Bitmap} rotation per frame. The natural-lock is
 * implemented on the primary path only; the legacy fallback keeps {@link Projection}'s
 * source-aspect geometry (pre-Android-12, not the modern target).
 *
 * <h2>Fill mode (protocol.md §5.3)</h2>
 *
 * <p>The primary mirror always aspect-<em>fits</em>, so for {@code scaleMode == fill}
 * the reader is instead sized to the natural-orientation COVER rectangle (the source
 * fills it exactly, no letterbox; see {@link Projection#fillCover}) and {@link #acquire}
 * yields that oversized frame. The centered {@code targetW×targetH} panel crop is then
 * taken by {@link FramePipeline} as the strip read-origin ({@link #cropX}/{@link #cropY})
 * — no extra per-frame copy, since stripping already sub-bitmaps each band. Fit (the
 * default) and the legacy fallback keep a panel-sized reader with a {@code (0,0)} crop.
 *
 * <p>All hidden-API calls go through reflection; a total failure (both paths)
 * surfaces as an exception that aborts the connection rather than silently
 * degrading. Use {@code --test-pattern} to exercise the pipeline without touching
 * these APIs.
 */
final class ScreenCapture {
    private int readerW;  // reader (capture) width  — panel oriented to the rotation
    private int readerH;  // reader (capture) height
    private ImageReader reader;

    // Degrees to rotate each captured frame to undo the device rotation and land in
    // the natural orientation (0 = no-op). Set on the primary path; 0 on the fallback.
    private int counterDeg;

    // Fill-mode centered crop of the panel out of the (oversized, cover-sized) frame
    // acquire() yields, in natural orientation. (0,0) for fit and the fallback path,
    // where acquire() is already exactly panel-sized.
    private int cropX;
    private int cropY;

    // Exactly one of these is non-null, depending on which creation path took.
    private VirtualDisplay virtualDisplay;  // primary: DisplayManager path
    private Class<?> sc;                     // fallback: android.view.SurfaceControl
    private IBinder display;                 // fallback: the SurfaceControl display token

    /**
     * @param rotation the device's current {@code Surface.ROTATION_*} code (0/1/2/3).
     *                 The capture is built for it and must be recreated when it changes.
     */
    ScreenCapture(int srcW, int srcH, int targetW, int targetH, int scaleMode, int rotation)
            throws Exception {
        // Natural-orientation content the reader holds: the panel for fit, the cover
        // rectangle for fill (the source fills it exactly, then we crop the panel out).
        int contentW, contentH;
        if (scaleMode == 1) {  // fill: oversize the reader to cover the panel
            int[] nat = Projection.naturalSize(srcW, srcH, rotation);
            int[] cov = Projection.fillCover(nat[0], nat[1], targetW, targetH);
            contentW = cov[0];
            contentH = cov[1];
            this.cropX = cov[2];
            this.cropY = cov[3];
        } else {  // fit: the reader is exactly the panel; no crop
            contentW = targetW;
            contentH = targetH;
            this.cropX = 0;
            this.cropY = 0;
        }
        boolean transposed = (rotation % 2) != 0;  // 90° / 270°: panel axes swap
        // Reader matches the content oriented to the rotation, so the rotated logical
        // display fills it; counter-rotate by the inverse rotation in acquire().
        this.readerW = transposed ? contentH : contentW;
        this.readerH = transposed ? contentW : contentH;
        this.counterDeg = (rotation & 3) * 90;  // undo the device rotation (direction verified on device)

        try {
            this.reader = ImageReader.newInstance(readerW, readerH, PixelFormat.RGBA_8888, 3);
            this.virtualDisplay = createVirtualDisplay(readerW, readerH, reader.getSurface());
            if (virtualDisplay == null) {
                throw new IllegalStateException("createVirtualDisplay returned null");
            }
        } catch (Throwable dmError) {
            // Older Android (no static DisplayManager.createVirtualDisplay): fall back
            // to the legacy SurfaceControl projection, which drives rotation/scale
            // itself via Projection (source-aspect, no natural-lock) into a panel-sized
            // reader — so acquire() must not counter-rotate that path.
            if (reader != null) reader.close();
            this.readerW = targetW;
            this.readerH = targetH;
            this.counterDeg = 0;
            this.cropX = 0;  // the fallback drives fit/fill via Projection into a panel-sized reader
            this.cropY = 0;
            this.reader = ImageReader.newInstance(targetW, targetH, PixelFormat.RGBA_8888, 3);
            try {
                createSurfaceControlDisplay(srcW, srcH, targetW, targetH, scaleMode, reader.getSurface());
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
     * surface; the system handles scale-fit / letterbox.
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
     * Latest captured frame as a panel-sized (targetW×targetH) ARGB_8888 bitmap, in
     * the device's natural orientation (the logical rotation undone — see the class
     * doc), already scaled / letterboxed by the GPU, or null if none is ready yet
     * (e.g. a static screen produces no new frames — the caller then sends nothing
     * and the Tab5 keeps showing the last frame).
     */
    Bitmap acquire() {
        Image img = reader.acquireLatestImage();
        if (img == null) return null;
        Bitmap frame;
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
            if (rowPadding == 0) {
                frame = padded;
            } else {
                frame = Bitmap.createBitmap(padded, 0, 0, readerW, readerH);
                if (frame != padded) padded.recycle();
            }
        } finally {
            img.close();
        }
        if (counterDeg == 0) return frame;  // ROTATION_0 (or fallback): already natural
        // Undo the device rotation → natural-orientation panel frame. A 90°/270°
        // rotation of a readerH×readerW (transposed) frame lands at targetW×targetH.
        Matrix m = new Matrix();
        m.postRotate(counterDeg);
        Bitmap rotated = Bitmap.createBitmap(frame, 0, 0, readerW, readerH, m, true);
        if (rotated != frame) frame.recycle();
        return rotated;
    }

    /** Natural-orientation X origin of the centered panel crop in {@link #acquire}'s frame (0 unless fill). */
    int cropX() { return cropX; }

    /** Natural-orientation Y origin of the centered panel crop in {@link #acquire}'s frame (0 unless fill). */
    int cropY() { return cropY; }

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
