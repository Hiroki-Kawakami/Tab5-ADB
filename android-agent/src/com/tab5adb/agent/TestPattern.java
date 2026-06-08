package com.tab5adb.agent;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;

/**
 * A deterministic source frame for verifying the mirror pipeline without real
 * screen capture (the test approach in android-agent/docs/testing.md §7 / the
 * mirror-phase2 test plan): a grid + an up-arrow + uniquely-coloured corner
 * blocks so the headless test can decode strips and eyeball rotation /
 * fit-vs-fill placement, while the structural asserts (dims, coverage,
 * 16-alignment) carry pass/fail.
 *
 * <p>No text is drawn: under a bare app_process there is no default Typeface, so
 * {@code Canvas.drawText} aborts. Coloured corner blocks (TL red, TR green, BL
 * blue, BR yellow) identify orientation instead. Enabled via {@code Server
 * --test-pattern}; the source size defaults to portrait and can be overridden
 * (e.g. landscape, to exercise rotation).
 */
final class TestPattern {
    private TestPattern() {}

    /** Draw a grid + up-arrow + coloured corner blocks at the given dimensions. */
    static Bitmap make(int w, int h, int frame) {
        Bitmap b = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(b);
        c.drawColor(Color.rgb(18, 18, 28));

        Paint grid = new Paint();
        grid.setColor(Color.rgb(80, 80, 100));
        grid.setStrokeWidth(2);
        for (int x = 0; x <= w; x += 100) c.drawLine(x, 0, x, h, grid);
        for (int y = 0; y <= h; y += 100) c.drawLine(0, y, w, y, grid);

        // Uniquely-coloured corner blocks — read orientation/placement after decode.
        int m = Math.max(40, Math.min(w, h) / 12);
        Paint p = new Paint();
        p.setColor(Color.rgb(230, 60, 60));   c.drawRect(0, 0, m, m, p);               // TL red
        p.setColor(Color.rgb(60, 200, 90));   c.drawRect(w - m, 0, w, m, p);           // TR green
        p.setColor(Color.rgb(70, 120, 240));  c.drawRect(0, h - m, m, h, p);           // BL blue
        p.setColor(Color.rgb(230, 200, 60));  c.drawRect(w - m, h - m, w, h, p);       // BR yellow

        // Centre up-arrow (points to the top of the source frame).
        Paint arrow = new Paint();
        arrow.setColor(Color.rgb(0, 210, 90));
        arrow.setStrokeWidth(14);
        arrow.setAntiAlias(true);
        int cx = w / 2, top = h / 4, bot = h * 3 / 4;
        c.drawLine(cx, bot, cx, top, arrow);
        c.drawLine(cx, top, cx - 70, top + 90, arrow);
        c.drawLine(cx, top, cx + 70, top + 90, arrow);

        // A moving marker so successive frames differ (liveness in the stream).
        Paint dot = new Paint();
        dot.setColor(Color.rgb(230, 80, 80));
        dot.setAntiAlias(true);
        int n = ((frame % 10) + 10) % 10;
        c.drawCircle(80 + n * (w - 160) / 9.0f, h / 2.0f, 28, dot);

        return b;
    }
}
