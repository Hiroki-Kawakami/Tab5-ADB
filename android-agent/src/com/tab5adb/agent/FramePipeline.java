package com.tab5adb.agent;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Matrix;

import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * The mirror video pipeline (android-agent/docs/protocol.md §5.1): take a source
 * screen {@link Bitmap}, rotate to portrait, scale to the Tab5 panel (fit/fill),
 * split into horizontal strips, and JPEG-encode each. The output is a list of
 * {@link Strip}s carrying the Tab5 device-coordinate rectangle (x,y,w,h, all 16px
 * multiples — §5.2) and the JPEG bytes.
 *
 * <p>This is the single source of the geometry for both real screen capture and
 * the deterministic {@link TestPattern} verification, so the headless test
 * exercises the exact pipeline the app uses.
 */
final class FramePipeline {
    /** One JPEG strip: a 16-aligned rect on the Tab5 panel + its JPEG bytes. */
    static final class Strip {
        int x, y, w, h;
        byte[] jpeg;
    }

    private final int targetW;
    private final int targetH;
    private final int scaleMode;  // 0 = fit, 1 = fill (§5.3)
    private final int split;
    private final int quality;

    FramePipeline(int targetW, int targetH, int scaleMode, int split, int quality) {
        this.targetW = targetW;
        this.targetH = targetH;
        this.scaleMode = scaleMode;
        this.split = Math.max(1, split);
        this.quality = quality;
    }

    /**
     * Strip-only entry for the GPU capture path: {@code frame} is already the final
     * targetW×targetH panel frame (rotate / scale-fit / letterbox done by the
     * compositor in {@link ScreenCapture}/{@link Projection}), so just split it into
     * strips + JPEG-encode. Every strip is full panel width (x=0, w=targetW), like
     * {@link #process}. Does not recycle {@code frame} (the caller owns it).
     */
    List<Strip> stripsOf(Bitmap frame) {
        return stripify(frame, 0, 0);
    }

    /**
     * Run the full CPU pipeline (rotate → scale-fit/fill → letterbox → strip). Used
     * by the deterministic {@link TestPattern} path, which has no SurfaceFlinger to
     * offload the geometry to. Does not recycle {@code src} (the caller owns it).
     */
    List<Strip> process(Bitmap src) {
        Bitmap rotated = rotateToPortrait(src);
        int rw = rotated.getWidth(), rh = rotated.getHeight();

        // Always emit a full target-sized frame (`frame` is exactly targetW x
        // targetH), so every strip spans the whole panel width (x=0, w=targetW).
        // The Tab5 then HW-JPEG-decodes each strip straight into its framebuffer
        // row band with no horizontal stride/offset — the ESP32-P4 2D-DMA can only
        // write a tightly-packed picture, so a narrower (letterboxed) strip could
        // not be placed into the wider panel buffer. `fill` already covers the
        // target; `fit` composites the inscribed image onto a black canvas here, so
        // the letterbox is baked into the JPEG (it compresses to almost nothing).
        Bitmap frame;
        if (scaleMode == 1) {  // fill: cover then center-crop to exactly target
            double s = Math.max((double) targetW / rw, (double) targetH / rh);
            int sw = Math.max(targetW, ceil16((int) Math.round(rw * s)));
            int sh = Math.max(targetH, ceil16((int) Math.round(rh * s)));
            Bitmap scaled = Bitmap.createScaledBitmap(rotated, sw, sh, true);
            int cx = floor16((sw - targetW) / 2);
            int cy = floor16((sh - targetH) / 2);
            frame = Bitmap.createBitmap(scaled, cx, cy, targetW, targetH);
            if (scaled != frame) scaled.recycle();
        } else {  // fit: aspect-preserve inscribe, center on a black letterbox canvas
            double s = Math.min((double) targetW / rw, (double) targetH / rh);
            int sw = clamp16((int) Math.round(rw * s), targetW);
            int sh = clamp16((int) Math.round(rh * s), targetH);
            Bitmap scaled = Bitmap.createScaledBitmap(rotated, sw, sh, true);
            int offX = floor16((targetW - sw) / 2);
            int offY = floor16((targetH - sh) / 2);
            frame = Bitmap.createBitmap(targetW, targetH, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(frame);
            c.drawColor(Color.BLACK);
            c.drawBitmap(scaled, offX, offY, null);  // drawBitmap needs no Typeface
            if (scaled != rotated) scaled.recycle();
        }
        if (rotated != src && rotated != frame) rotated.recycle();

        List<Strip> strips = stripify(frame, 0, 0);  // full-width strips (offX=offY=0)
        if (frame != src) frame.recycle();
        return strips;
    }

    /** §5.1 step 1: landscape sources rotate 270° so the result is always portrait. */
    private static Bitmap rotateToPortrait(Bitmap src) {
        if (src.getWidth() <= src.getHeight()) return src;  // already portrait
        Matrix m = new Matrix();
        m.postRotate(270);
        return Bitmap.createBitmap(src, 0, 0, src.getWidth(), src.getHeight(), m, true);
    }

    /** §5.1 step 3+4: split into `split` horizontal bands (each 16-aligned) + encode. */
    private List<Strip> stripify(Bitmap image, int offX, int offY) {
        int imgW = image.getWidth(), imgH = image.getHeight();
        int base = Math.max(16, floor16(imgH / split));
        List<Strip> out = new ArrayList<>(split);
        int y = 0;
        for (int i = 0; i < split && y < imgH; i++) {
            // Last band absorbs the remainder so the strips sum to exactly imgH;
            // imgH and base are 16-multiples, so the remainder is too.
            int h = (i == split - 1) ? (imgH - y) : Math.min(base, imgH - y);
            Bitmap band = Bitmap.createBitmap(image, 0, y, imgW, h);
            Strip st = new Strip();
            st.x = offX;
            st.y = offY + y;
            st.w = imgW;
            st.h = h;
            st.jpeg = encode(band);
            band.recycle();
            out.add(st);
            y += h;
        }
        return out;
    }

    private byte[] encode(Bitmap band) {
        ByteArrayOutputStream baos = new ByteArrayOutputStream(band.getWidth() * 8);
        // Android's JPEG encoder uses 4:2:0 subsampling at this quality, matching
        // the Tab5 HW JPEG decoder's YUV420 expectation (§5.1).
        band.compress(Bitmap.CompressFormat.JPEG, quality, baos);
        return baos.toByteArray();
    }

    // --- 16px alignment helpers (YUV420 MCU + HW JPEG, §5.2) ---
    private static int floor16(int v) { return (v / 16) * 16; }
    private static int ceil16(int v) { return ((v + 15) / 16) * 16; }
    /** Floor to 16 and clamp to <= max (also a 16-multiple), min 16. */
    private static int clamp16(int v, int max) { return Math.max(16, Math.min(floor16(v), max)); }
}
