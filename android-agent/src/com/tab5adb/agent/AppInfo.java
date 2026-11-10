/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.util.DisplayMetrics;

import java.io.ByteArrayOutputStream;
import java.lang.reflect.Array;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

// The GET_APP_INFO result: each app's name and launcher icon.
final class AppInfo {
    private static final int MATCH_DISABLED =
            PackageManager.MATCH_DISABLED_COMPONENTS | PackageManager.MATCH_DISABLED_UNTIL_USED_COMPONENTS;

    private final PackageManager packages;
    private final TextRender text;

    private AppInfo(PackageManager packages, TextRender text) {
        this.packages = packages;
        this.text = text;
    }

    static AppInfo create(TextRender text) throws Exception {
        PackageManager packages = SystemContext.get().getPackageManager();
        if (packages == null) {
            throw new IllegalStateException("no PackageManager");
        }
        return new AppInfo(packages, text);
    }

    void write(ByteArrayOutputStream out, String name, int iconPx, int textPx) {
        ApplicationInfo app;
        try {
            app = packages.getApplicationInfo(name, MATCH_DISABLED);
        } catch (PackageManager.NameNotFoundException e) {
            out.write(0);
            out.write(0);
            out.write(0);
            out.write(0);
            return;
        }
        out.write(1);
        out.write(0);
        out.write(0);
        out.write(0);
        text.write(out, label(app), textPx);
        int[] pixels = iconPx > 0 ? icon(app, iconPx) : null;
        int size = pixels != null ? iconPx : 0;
        out.write(size);
        out.write(size >> 8);
        out.write(size);
        out.write(size >> 8);
        if (pixels == null) {
            return;
        }
        for (int pixel : pixels) {
            out.write(pixel);
            out.write(pixel >> 8);
            out.write(pixel >> 16);
            out.write(pixel >>> 24);
        }
    }

    private String label(ApplicationInfo app) {
        try {
            CharSequence label = packages.getApplicationLabel(app);
            String trimmed = label != null ? label.toString().trim() : "";
            if (!trimmed.isEmpty()) {
                return trimmed;
            }
        } catch (Throwable t) {
            System.out.println("label " + app.packageName + ": " + t);
        }
        return app.packageName;
    }

    private int[] icon(ApplicationInfo app, int size) {
        try {
            Drawable drawable = null;
            if (app.icon != 0) {
                try {
                    drawable = withSplits(app).getDrawableForDensity(app.icon, DisplayMetrics.DENSITY_XXHIGH, null);
                } catch (Throwable t) {
                    System.out.println("icon " + app.packageName + ": " + t);
                }
            }
            if (drawable == null) {
                drawable = packages.getApplicationIcon(app);
            }
            Bitmap bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
            drawable.setBounds(0, 0, size, size);
            drawable.draw(new Canvas(bitmap));
            int[] pixels = new int[size * size];
            bitmap.getPixels(pixels, 0, size, 0, 0, size, size);
            bitmap.recycle();
            return pixels;
        } catch (Throwable t) {
            System.out.println("icon " + app.packageName + ": " + t);
            return null;
        }
    }

    // Split installs keep the launcher icon in a split_config.<dpi> APK, which the system
    // Context's resources do not load; AssetManager.addAssetPath would keep each split's table
    // apart, so mount them the way the framework does, with ApkAssets and setApkAssets.
    private static Resources withSplits(ApplicationInfo app) throws Exception {
        Class<?> apkAssets = Class.forName("android.content.res.ApkAssets");
        Method load = apkAssets.getMethod("loadFromPath", String.class);
        List<Object> loaded = new ArrayList<>();
        loaded.add(load.invoke(null, "/system/framework/framework-res.apk"));
        loaded.add(load.invoke(null, app.publicSourceDir));
        if (app.splitPublicSourceDirs != null) {
            for (String split : app.splitPublicSourceDirs) {
                loaded.add(load.invoke(null, split));
            }
        }
        Object array = Array.newInstance(apkAssets, loaded.size());
        for (int i = 0; i < loaded.size(); i++) {
            Array.set(array, i, loaded.get(i));
        }
        AssetManager assets = AssetManager.class.getDeclaredConstructor().newInstance();
        AssetManager.class.getMethod("setApkAssets", array.getClass(), boolean.class).invoke(assets, array, true);
        DisplayMetrics metrics = new DisplayMetrics();
        metrics.setToDefaults();
        Configuration configuration = new Configuration();
        configuration.setToDefaults();
        return new Resources(assets, metrics, configuration);
    }
}
