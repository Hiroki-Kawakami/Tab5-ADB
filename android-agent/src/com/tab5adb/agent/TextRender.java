package com.tab5adb.agent;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.text.TextPaint;
import android.text.TextUtils;

import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/**
 * Rasterizes a single line of text to an 8-bit alpha mask (A8) for the Tab5 to
 * tint and blit — so the Tab5 needs no CJK/other-script fonts and the media
 * card's title/artist render for any language.
 *
 * <p><b>The bare-app_process Typeface pitfall</b> (verified on a real device):
 * app_process never loads the system font map, so {@code Typeface.DEFAULT} has a
 * null native instance and every legacy path ({@code new Paint()} default,
 * {@code Typeface.createFromFile}, {@code CustomFallbackBuilder.build}) aborts or
 * throws "Typeface is not fully initialized". We sidestep it: build font families
 * with the low-level {@code android.graphics.fonts} API and call the hidden static
 * {@code Typeface.nativeCreateFromArray(famPtrs, fallback=0, weight=-1, italic=0)}
 * directly, yielding a self-contained Typeface that never reads {@code DEFAULT}.
 * That typeface is set explicitly on every Paint, so glyph resolution never
 * touches the (null) default either.
 */
final class TextRender {
    private final Typeface typeface;

    private TextRender(Typeface tf) {
        this.typeface = tf;
    }

    /** A rasterized line: an A8 alpha mask, {@code w*h} bytes (1 byte/px). */
    static final class Line {
        final int w, h;
        final byte[] alpha;
        Line(int w, int h, byte[] alpha) { this.w = w; this.h = h; this.alpha = alpha; }
    }

    /**
     * Build the renderer with a Latin-primary + broad-script fallback chain from
     * /system/fonts. Throws if no usable font is found (the caller then renders
     * text sections as absent).
     */
    static TextRender create() throws Exception {
        // Primary first (Latin/digits), then fallback families covering CJK etc.
        // Best-effort: only existing files are added, in priority order.
        String[] candidates = {
            "/system/fonts/Roboto-Regular.ttf",
            "/system/fonts/NotoSansCJK-Regular.ttc",
            "/system/fonts/NotoSansCJKjp-Regular.otf",
            "/system/fonts/NotoSans-Regular.ttf",
            "/system/fonts/NotoSansArabic-Regular.ttf",
            "/system/fonts/NotoSansHebrew-Regular.ttf",
            "/system/fonts/NotoSansThai-Regular.ttf",
            "/system/fonts/NotoSansDevanagari-Regular.ttf",
            "/system/fonts/NotoSansSymbols-Regular-Subsetted.ttf",
            "/system/fonts/DroidSansFallback.ttf",
        };
        List<String> files = new ArrayList<>();
        for (String f : candidates) if (new File(f).exists()) files.add(f);
        if (files.isEmpty()) throw new IllegalStateException("no usable /system/fonts file");
        Typeface tf = buildTypeface(files.toArray(new String[0]));
        return new TextRender(tf);
    }

    /**
     * Render one line to an A8 mask, ellipsized (…) to {@code maxWidth} px.
     * Returns null for empty text.
     */
    Line render(String text, int textPx, int maxWidth) {
        if (text == null || text.isEmpty() || maxWidth <= 0 || textPx <= 0) return null;
        TextPaint p = new TextPaint(Paint.ANTI_ALIAS_FLAG);
        p.setTextSize(textPx);
        p.setColor(0xFFFFFFFF);
        p.setTypeface(typeface);
        CharSequence ell = TextUtils.ellipsize(text, p, maxWidth, TextUtils.TruncateAt.END);
        String s = ell.toString();
        if (s.isEmpty()) return null;
        int w = Math.max(1, Math.min(maxWidth, (int) Math.ceil(p.measureText(s))));
        Paint.FontMetricsInt fm = p.getFontMetricsInt();
        int h = Math.max(1, fm.descent - fm.ascent);
        Bitmap bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ALPHA_8);
        Canvas c = new Canvas(bmp);
        c.drawText(s, 0, -fm.ascent, p);
        int[] px = new int[w * h];
        bmp.getPixels(px, 0, w, 0, 0, w, h);  // ALPHA_8: alpha is in bits 24..31
        bmp.recycle();
        byte[] a = new byte[w * h];
        for (int i = 0; i < px.length; i++) a[i] = (byte) (px[i] >>> 24);
        return new Line(w, h, a);
    }

    /** Build a self-contained Typeface from font files (first = primary, rest = fallback). */
    private static Typeface buildTypeface(String[] files) throws Exception {
        Class<?> fontBuilderC = Class.forName("android.graphics.fonts.Font$Builder");
        Class<?> fontC = Class.forName("android.graphics.fonts.Font");
        Class<?> famBuilderC = Class.forName("android.graphics.fonts.FontFamily$Builder");
        Class<?> famC = Class.forName("android.graphics.fonts.FontFamily");

        Constructor<?> fontBuilderCtor = fontBuilderC.getConstructor(File.class);
        Method fontBuild = fontBuilderC.getMethod("build");
        Constructor<?> famBuilderCtor = famBuilderC.getConstructor(fontC);
        Method famBuild = famBuilderC.getMethod("build");
        Method getNativePtr = famC.getMethod("getNativePtr");  // hidden, returns long

        List<Long> ptrs = new ArrayList<>();
        for (String f : files) {
            try {
                Object font = fontBuild.invoke(fontBuilderCtor.newInstance(new File(f)));
                Object family = famBuild.invoke(famBuilderCtor.newInstance(font));
                ptrs.add((Long) getNativePtr.invoke(family));
            } catch (Throwable t) {
                System.err.println("tab5adb-agent: font skip " + f + ": " + t);
            }
        }
        if (ptrs.isEmpty()) throw new IllegalStateException("no font family built");
        long[] famPtrs = new long[ptrs.size()];
        for (int i = 0; i < famPtrs.length; i++) famPtrs[i] = ptrs.get(i);

        // nativeCreateFromArray(long[] families, long fallback, int weight, int italic).
        // fallback=0 => the typeface stands alone (never reads Typeface.DEFAULT).
        Method nca = null;
        for (Method m : Typeface.class.getDeclaredMethods()) {
            if (m.getName().equals("nativeCreateFromArray")) { nca = m; break; }
        }
        if (nca == null) throw new IllegalStateException("no Typeface.nativeCreateFromArray");
        nca.setAccessible(true);
        Class<?>[] pt = nca.getParameterTypes();
        Object[] call = new Object[pt.length];
        call[0] = famPtrs;
        for (int i = 1; i < pt.length; i++) {
            if (pt[i] == long.class) call[i] = 0L;            // fallback typeface ptr
            else call[i] = (i == pt.length - 1) ? 0 : -1;     // italic=0, weight=-1 (RESOLVE_BY_FONT_TABLE)
        }
        long ni = (Long) nca.invoke(null, call);

        Constructor<Typeface> tfCtor = Typeface.class.getDeclaredConstructor(long.class);
        tfCtor.setAccessible(true);
        return tfCtor.newInstance(ni);
    }
}
