package com.tab5adb.agent;

/**
 * Pure geometry for the GPU-offloaded capture path (android-agent/docs/protocol.md
 * §5.1): given the source display size and the Tab5 target panel, compute the
 * virtual-display projection that SurfaceFlinger applies on the GPU — a rotation
 * plus the rectangle the source lands in inside the fixed targetW×targetH output
 * surface. The area outside that rectangle is the black letterbox, filled by the
 * virtual display's own background (no CPU composite).
 *
 * <p>This holds the rotate / scale-fit / center arithmetic that used to run on the
 * CPU in {@link FramePipeline} (and was verified by the headless test). It has no
 * android imports, so the arithmetic is unit-testable on the host JVM (see
 * {@code test/ProjectionTest.java}); the GPU result itself — black letterbox,
 * rotation direction — is only verifiable on a real device.
 *
 * <p>{@code orientation} is the {@code setDisplayProjection} rotation code (0/1/2/3
 * = 0°/90°/180°/270°). {@code layerStack*} is the captured source region (the full
 * source in natural coords). {@code disp*} is the destination rectangle in the
 * targetW×targetH output surface.
 */
final class Projection {
    final int orientation;
    final int layerStackL, layerStackT, layerStackR, layerStackB;
    final int dispL, dispT, dispR, dispB;

    private Projection(int orientation, int lsl, int lst, int lsr, int lsb,
                       int dl, int dt, int dr, int db) {
        this.orientation = orientation;
        this.layerStackL = lsl; this.layerStackT = lst; this.layerStackR = lsr; this.layerStackB = lsb;
        this.dispL = dl; this.dispT = dt; this.dispR = dr; this.dispB = db;
    }

    /** scaleMode: 0 = fit (aspect-preserve inscribe + letterbox), 1 = fill (cover + center-crop). */
    static Projection compute(int srcW, int srcH, int targetW, int targetH, int scaleMode) {
        boolean landscape = srcW > srcH;
        // Rotate a landscape source 270° so the mirror is upright portrait, matching
        // the CPU path's postRotate(270); portrait sources need no rotation. The
        // common case (portrait phone) is orientation 0 — already verified visually;
        // the landscape CW/CCW direction is a single constant to confirm on device.
        int orientation = landscape ? 3 : 0;
        int rw = landscape ? srcH : srcW;  // source dims after the rotation
        int rh = landscape ? srcW : srcH;
        double s = (scaleMode == 1)
                ? Math.max((double) targetW / rw, (double) targetH / rh)   // fill: cover the panel
                : Math.min((double) targetW / rw, (double) targetH / rh);  // fit: inscribe in the panel
        int dw = (int) Math.round(rw * s);
        int dh = (int) Math.round(rh * s);
        int offX = (targetW - dw) / 2;  // negative for fill (overflows; clipped by the output surface)
        int offY = (targetH - dh) / 2;
        return new Projection(orientation, 0, 0, srcW, srcH,
                              offX, offY, offX + dw, offY + dh);
    }
}
