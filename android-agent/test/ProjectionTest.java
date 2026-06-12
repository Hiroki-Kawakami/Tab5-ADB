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

        // fillCover (primary-path fill geometry, §5.3): the oversized natural-orientation
        // reader the source fills exactly, + the centered panel crop origin. 1080x2160
        // into 720x1280: s=max(.667,.593)=.667 → cover 720x1440, crop the centered
        // 720x1280 (cropY=80) — the same center-crop fill() projects, done by sizing the
        // reader + reading an off-center band instead of a negative dest offset.
        eq("fillCover portrait", "720,1440,0,80",
                arr(Projection.fillCover(1080, 2160, 720, 1280)));
        // Wider-than-panel natural source crops horizontally instead: 1080x1600 →
        // s=max(.667,.8)=.8 → cover 864x1280, cropX=72.
        eq("fillCover wide", "864,1280,72,0",
                arr(Projection.fillCover(1080, 1600, 720, 1280)));
        // Exact panel aspect: no oversize, no crop (what Adapt's `wm size` yields).
        eq("fillCover exact", "720,1280,0,0",
                arr(Projection.fillCover(1080, 1920, 720, 1280)));

        // --- aspect scale mode output sizing (§5.3 aspect) ---

        // 1080x2340 into the 360x860 preview box: width-bound,
        // s = 1/3 exactly → 360x780. The fixed preview width lands on the box edge.
        eq("aspectOutput tall", "360,780", arr(Projection.aspectOutput(1080, 2340, 360, 860)));
        // 16:9 1080x1920: s = 1/3 → 360x640.
        eq("aspectOutput 16:9", "360,640", arr(Projection.aspectOutput(1080, 1920, 360, 860)));
        // Very tall 1080x2700: height-bound, s = .31852 → 344x860 (width rounds even,
        // height hits the box).
        eq("aspectOutput heightbound", "344,860", arr(Projection.aspectOutput(1080, 2700, 360, 860)));
        // Landscape-natural source (tablet): width-bound, short output (225 -> even 226).
        eq("aspectOutput landscape", "360,226", arr(Projection.aspectOutput(2560, 1600, 360, 860)));
        // A box-aspect source fills the box exactly (identity at scale 1).
        eq("aspectOutput exact", "360,860", arr(Projection.aspectOutput(360, 860, 360, 860)));

        // --- touch passthrough inverse mapping (§4.7) ---

        // naturalSize un-rotates the current-rotation source size to portrait.
        eq("naturalSize rot0", "1080,2160", arr(Projection.naturalSize(1080, 2160, 0)));
        eq("naturalSize rot90", "1080,2160", arr(Projection.naturalSize(2160, 1080, 1)));
        eq("naturalSize rot180", "1080,2160", arr(Projection.naturalSize(1080, 2160, 2)));
        eq("naturalSize rot270", "1080,2160", arr(Projection.naturalSize(2160, 1080, 3)));

        // Portrait fit (natW=1080,natH=2160 into 720x1280): s=.5926, disp 40,0..680,1280.
        // The image center panel(360,640) maps to source center (540,1080) at rot0,
        // and rotates with the device for the other rotations (logical coords).
        eq("inv rot0 center", "540,1080",
                arr(Projection.panelToLogical(360, 640, 1080, 2160, 720, 1280, 0, 0)));
        eq("inv rot90 center", "1080,539",
                arr(Projection.panelToLogical(360, 640, 1080, 2160, 720, 1280, 0, 1)));
        eq("inv rot180 center", "539,1079",
                arr(Projection.panelToLogical(360, 640, 1080, 2160, 720, 1280, 0, 2)));
        eq("inv rot270 center", "1079,540",
                arr(Projection.panelToLogical(360, 640, 1080, 2160, 720, 1280, 0, 3)));

        // Corners of the image rectangle map to source corners (rot0).
        eq("inv rot0 topleft", "0,0",
                arr(Projection.panelToLogical(40, 0, 1080, 2160, 720, 1280, 0, 0)));

        // Left/right letterbox bands have no source pixel -> null.
        eq("inv letterbox left", "null",
                arr(Projection.panelToLogical(10, 640, 1080, 2160, 720, 1280, 0, 0)));
        eq("inv letterbox right", "null",
                arr(Projection.panelToLogical(710, 640, 1080, 2160, 720, 1280, 0, 0)));

        // Fill mode (no letterbox): the whole panel maps inside the source. A point
        // near the top edge that fit would clip is valid here. s=.667, offY=-80.
        eq("inv fill top", "540,120",
                arr(Projection.panelToLogical(360, 0, 1080, 2160, 720, 1280, 1, 0)));

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

    private static String arr(int[] a) {
        if (a == null) return "null";
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < a.length; i++) {
            if (i > 0) sb.append(',');
            sb.append(a[i]);
        }
        return sb.toString();
    }

    private static void eq(String what, Object expected, Object actual) {
        if (!expected.equals(actual)) {
            System.out.println("  FAIL " + what + ": expected " + expected + ", got " + actual);
            failures++;
        }
    }
}
