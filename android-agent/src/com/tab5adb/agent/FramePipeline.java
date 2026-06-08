package com.tab5adb.agent;

import android.graphics.Bitmap;
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

    /** Run the pipeline. Does not recycle {@code src} (the caller owns it). */
    List<Strip> process(Bitmap src) {
        Bitmap rotated = rotateToPortrait(src);
        // Scale to the panel; `image` is the placed image, (offX,offY) its 16-aligned
        // top-left on the panel.
        Bitmap image;
        int offX, offY;
        int rw = rotated.getWidth(), rh = rotated.getHeight();
        if (scaleMode == 1) {  // fill: cover then center-crop to exactly target
            double s = Math.max((double) targetW / rw, (double) targetH / rh);
            int sw = Math.max(targetW, ceil16((int) Math.round(rw * s)));
            int sh = Math.max(targetH, ceil16((int) Math.round(rh * s)));
            Bitmap scaled = Bitmap.createScaledBitmap(rotated, sw, sh, true);
            int cx = floor16((sw - targetW) / 2);
            int cy = floor16((sh - targetH) / 2);
            image = Bitmap.createBitmap(scaled, cx, cy, targetW, targetH);
            if (scaled != image) scaled.recycle();
            offX = 0;
            offY = 0;
        } else {  // fit: aspect-preserve inscribe, center with letterbox
            double s = Math.min((double) targetW / rw, (double) targetH / rh);
            int sw = clamp16((int) Math.round(rw * s), targetW);
            int sh = clamp16((int) Math.round(rh * s), targetH);
            image = Bitmap.createScaledBitmap(rotated, sw, sh, true);
            offX = floor16((targetW - sw) / 2);
            offY = floor16((targetH - sh) / 2);
        }
        if (rotated != src && rotated != image) rotated.recycle();

        List<Strip> strips = stripify(image, offX, offY);
        if (image != src) image.recycle();
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
