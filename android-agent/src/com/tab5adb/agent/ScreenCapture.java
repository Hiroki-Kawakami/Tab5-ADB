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
 * Bitmap} for {@link FramePipeline} to split into strips.
 *
 * <p>Geometry (rotate / scale-fit / letterbox) is offloaded to the GPU: the
 * virtual display is projected straight into a fixed targetW×targetH (Tab5 panel)
 * {@link ImageReader} via {@code setDisplayProjection} (rotation code + a centered
 * destination rect, the rest left as the display's black background), per {@link
 * Projection}. So {@code acquire()} already returns the final upright, scaled,
 * letterboxed panel-sized frame — no CPU readback at source resolution and no
 * rotate/scale/composite Bitmap copies. {@link FramePipeline} then only strips +
 * JPEG-encodes. (The {@link TestPattern} path still runs the CPU geometry in
 * {@link FramePipeline}, since the GPU projection needs a real SurfaceFlinger.)
 *
 * <p>All {@code SurfaceControl} calls go through reflection (the class is hidden);
 * a failure here surfaces as an exception that aborts the connection rather than
 * silently degrading. Use {@code --test-pattern} to exercise the pipeline without
 * touching these APIs.
 */
final class ScreenCapture {
    private final int readerW;  // = target panel width (the projected output size)
    private final int readerH;
    private final Class<?> sc;  // android.view.SurfaceControl
    private final ImageReader reader;
    private final IBinder display;

    ScreenCapture(int srcW, int srcH, int targetW, int targetH, int scaleMode) throws Exception {
        this.readerW = targetW;
        this.readerH = targetH;
        this.sc = Class.forName("android.view.SurfaceControl");
        // The reader is the Tab5 panel size; the compositor scales the source into it.
        this.reader = ImageReader.newInstance(targetW, targetH, PixelFormat.RGBA_8888, 3);

        IBinder disp = (IBinder) sc.getMethod("createDisplay", String.class, boolean.class)
                .invoke(null, "tab5adb-capture", false);
        if (disp == null) {
            reader.close();
            throw new IllegalStateException("SurfaceControl.createDisplay returned null");
        }
        this.display = disp;

        // GPU projection: rotate the source upright and scale-fit it into a centered
        // rect inside the panel-sized reader; the area outside the rect is the
        // virtual display's black background = the letterbox (no CPU composite).
        Projection p = Projection.compute(srcW, srcH, targetW, targetH, scaleMode);
        Surface surface = reader.getSurface();
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
