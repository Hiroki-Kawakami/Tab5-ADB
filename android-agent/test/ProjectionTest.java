package com.tab5adb.agent;

/**
 * Host-JVM unit test for {@link Projection} — the rotate / scale-fit / center math
 * that moved from the CPU {@link FramePipeline} onto the GPU
 * (setDisplayProjection). Projection has no android deps, so this runs on the
 * plain JDK with no device:
 *
 *   nix develop -c android-agent/test/run.sh
 *
 * The GPU result itself (black letterbox, rotation direction on a real landscape
 * source) is only verifiable on a device via the simverify mirror capture; this
 * pins the arithmetic that picks the projection rectangles. It shares Projection's
 * package to reach the package-private class + fields.
 */
public final class ProjectionTest {
    private static int failures = 0;

    public static void main(String[] args) {
        // Portrait source, fit into the 720x1280 panel: no rotation, full width,
        // letterboxed left/right. (1080x2160 source.)
        Projection p = Projection.compute(1080, 2160, 720, 1280, 0);
        eq("portrait orientation", 0, p.orientation);
        eq("portrait layerStack", "0,0,1080,2160", rect(p.layerStackL, p.layerStackT, p.layerStackR, p.layerStackB));
        // s = min(720/1080, 1280/2160) = .593 → 640x1280, x-centered.
        eq("portrait disp", "40,0,680,1280", rect(p.dispL, p.dispT, p.dispR, p.dispB));

        // Equal-aspect portrait source fits exactly — full panel, no letterbox.
        Projection exact = Projection.compute(720, 1280, 720, 1280, 0);
        eq("exact orientation", 0, exact.orientation);
        eq("exact disp", "0,0,720,1280", rect(exact.dispL, exact.dispT, exact.dispR, exact.dispB));

        // Landscape source rotates upright (270°) then fits: rotated dims (srcH,srcW),
        // so 2160x1080 → rotated 1080x2160 → same placement as the portrait case.
        Projection land = Projection.compute(2160, 1080, 720, 1280, 0);
        eq("landscape orientation", 3, land.orientation);
        eq("landscape layerStack", "0,0,2160,1080", rect(land.layerStackL, land.layerStackT, land.layerStackR, land.layerStackB));
        eq("landscape disp", "40,0,680,1280", rect(land.dispL, land.dispT, land.dispR, land.dispB));

        // Fill covers the panel: the larger scale, so one axis overflows and is
        // clipped (negative offset) → center-crop. 1080x2160 fill → s = max(.667,.593)
        // = .667 → 720x1440, y overflows by 80 each side.
        Projection fill = Projection.compute(1080, 2160, 720, 1280, 1);
        eq("fill orientation", 0, fill.orientation);
        eq("fill disp", "0,-80,720,1360", rect(fill.dispL, fill.dispT, fill.dispR, fill.dispB));

        if (failures == 0) {
            System.out.println("ProjectionTest: PASSED");
        } else {
            System.out.println("ProjectionTest: FAILED (" + failures + ")");
            System.exit(1);
        }
    }

    private static String rect(int l, int t, int r, int b) {
        return l + "," + t + "," + r + "," + b;
    }

    private static void eq(String what, Object expected, Object actual) {
        if (!expected.equals(actual)) {
            System.out.println("  FAIL " + what + ": expected " + expected + ", got " + actual);
            failures++;
        }
    }
}
