/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.text.BreakIterator;
import java.util.ArrayList;
import java.util.List;

// Draws strings into TEXT_IMAGE with the device's fonts.
final class TextRender {
    private static final int MAX_WIDTH = 2048;
    private static final String[] FONTS = {
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

    private final Typeface typeface;

    private TextRender(Typeface typeface) {
        this.typeface = typeface;
    }

    static TextRender create() throws Exception {
        List<String> files = new ArrayList<>();
        for (String file : FONTS) {
            if (new File(file).exists()) {
                files.add(file);
            }
        }
        if (files.isEmpty()) {
            throw new IllegalStateException("no fonts");
        }
        return new TextRender(typeface(files));
    }

    void write(ByteArrayOutputStream out, String text, int textPx) {
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setTypeface(typeface);
        paint.setTextSize(textPx);
        Paint.FontMetricsInt metrics = paint.getFontMetricsInt();
        int height = metrics.descent - metrics.ascent;
        List<Integer> breaks = new ArrayList<>();
        BreakIterator characters = BreakIterator.getCharacterInstance();
        characters.setText(text);
        int end = 0;
        for (int at = characters.next(); at != BreakIterator.DONE; at = characters.next()) {
            int x = (int) Math.ceil(paint.measureText(text, 0, at));
            if (x > MAX_WIDTH) {
                break;
            }
            breaks.add(x);
            end = at;
        }
        int width = breaks.isEmpty() ? 0 : breaks.get(breaks.size() - 1);
        short16(out, width);
        short16(out, height);
        short16(out, -metrics.ascent);
        short16(out, breaks.size());
        for (int x : breaks) {
            short16(out, x);
        }
        if (width == 0 || height <= 0) {
            return;
        }
        Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ALPHA_8);
        new Canvas(bitmap).drawText(text, 0, end, 0, -metrics.ascent, paint);
        int[] pixels = new int[width * height];
        bitmap.getPixels(pixels, 0, width, 0, 0, width, height);
        bitmap.recycle();
        for (int pixel : pixels) {
            out.write(pixel >>> 24);
        }
    }

    private static void short16(ByteArrayOutputStream out, int value) {
        out.write(value);
        out.write(value >> 8);
    }

    // Typeface.DEFAULT has no native instance under app_process, and every public way to make
    // a Typeface falls back to it; nativeCreateFromArray with no fallback does not.
    private static Typeface typeface(List<String> files) throws Exception {
        Class<?> fontBuilder = Class.forName("android.graphics.fonts.Font$Builder");
        Class<?> font = Class.forName("android.graphics.fonts.Font");
        Class<?> familyBuilder = Class.forName("android.graphics.fonts.FontFamily$Builder");
        Class<?> family = Class.forName("android.graphics.fonts.FontFamily");
        Constructor<?> newFont = fontBuilder.getConstructor(File.class);
        Method buildFont = fontBuilder.getMethod("build");
        Constructor<?> newFamily = familyBuilder.getConstructor(font);
        Method buildFamily = familyBuilder.getMethod("build");
        Method nativePtr = family.getMethod("getNativePtr");
        List<Long> families = new ArrayList<>();
        for (String file : files) {
            try {
                Object built = buildFont.invoke(newFont.newInstance(new File(file)));
                families.add((Long) nativePtr.invoke(buildFamily.invoke(newFamily.newInstance(built))));
            } catch (Throwable t) {
                System.out.println("font " + file + ": " + t);
            }
        }
        if (families.isEmpty()) {
            throw new IllegalStateException("no font families");
        }
        long[] pointers = new long[families.size()];
        for (int i = 0; i < pointers.length; i++) {
            pointers[i] = families.get(i);
        }
        Method create = null;
        for (Method method : Typeface.class.getDeclaredMethods()) {
            if (method.getName().equals("nativeCreateFromArray")) {
                create = method;
                break;
            }
        }
        if (create == null) {
            throw new IllegalStateException("no Typeface.nativeCreateFromArray");
        }
        create.setAccessible(true);
        Class<?>[] types = create.getParameterTypes();
        Object[] args = new Object[types.length];
        args[0] = pointers;
        for (int i = 1; i < types.length; i++) {
            args[i] = types[i] == long.class ? (Object) 0L : (Object) (i == types.length - 1 ? 0 : -1);
        }
        Constructor<Typeface> wrap = Typeface.class.getDeclaredConstructor(long.class);
        wrap.setAccessible(true);
        return wrap.newInstance((Long) create.invoke(null, args));
    }
}
