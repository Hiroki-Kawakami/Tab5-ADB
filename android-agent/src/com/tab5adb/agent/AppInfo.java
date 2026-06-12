package com.tab5adb.agent;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * App metadata for the Tab5 AppManager (protocol.md §4.4 GET_APP_LIST /
 * GET_APP_ICON): installed packages with human-readable labels + flags, and
 * launcher icons rendered to raw ARGB8888. Runs under app_process via the
 * {@link SystemContext} {@code PackageManager} (shell uid sees all packages —
 * it holds QUERY_ALL_PACKAGES).
 *
 * <p>Icons are drawn onto a {@link Canvas} (no text, so the bare-app_process
 * Typeface pitfall doesn't apply) and returned as Android {@code Color} ints
 * written little-endian — byte order B,G,R,A, which is also LVGL's native
 * ARGB8888, so neither side converts.
 */
final class AppInfo {
    private final PackageManager pm;

    private AppInfo(PackageManager pm) {
        this.pm = pm;
    }

    /** Build the service; throws if the PackageManager is unreachable. */
    static AppInfo create() throws Exception {
        PackageManager pm = SystemContext.get().getPackageManager();
        if (pm == null) throw new IllegalStateException("no PackageManager");
        return new AppInfo(pm);
    }

    private static final class Entry {
        int flags;
        byte[] pkg;
        byte[] label;
        String sortKey;
    }

    /** The GET_APP_LIST result payload (§4.4): count + var-length entries. */
    byte[] appListPayload() {
        List<ApplicationInfo> apps = pm.getInstalledApplications(0);
        List<Entry> entries = new ArrayList<>(apps.size());
        for (ApplicationInfo ai : apps) {
            Entry e = new Entry();
            e.flags = ((ai.flags & ApplicationInfo.FLAG_SYSTEM) != 0 ? 0x01 : 0)
                    | (ai.enabled ? 0 : 0x02);
            e.pkg = utf8Capped(ai.packageName);
            String label;
            try {
                CharSequence l = pm.getApplicationLabel(ai);
                // NBSP -> space: some labels (e.g. "Google Wallet") carry it
                // and the Tab5 fonts have no glyph for it.
                label = l != null ? l.toString().replace(' ', ' ').trim() : "";
            } catch (Throwable t) {
                label = "";  // per-app label failure: the Tab5 shows the package
            }
            if (label.equals(ai.packageName)) label = "";  // label_len 0 = use pkg
            e.label = utf8Capped(label);
            e.sortKey = (label.isEmpty() ? ai.packageName : label);
            entries.add(e);
        }
        // Sorted here (case-insensitive label order) so the Tab5 doesn't re-sort.
        Collections.sort(entries, (a, b) -> a.sortKey.compareToIgnoreCase(b.sortKey));

        ByteArrayOutputStream out = new ByteArrayOutputStream(entries.size() * 48 + 2);
        int count = entries.size();
        out.write(count & 0xFF);
        out.write((count >> 8) & 0xFF);
        for (Entry e : entries) {
            out.write(e.flags);
            out.write(0);  // reserved
            out.write(e.pkg.length);
            out.write(e.label.length);
            out.write(e.pkg, 0, e.pkg.length);
            out.write(e.label, 0, e.label.length);
        }
        return out.toByteArray();
    }

    /**
     * The GET_APP_ICON result payload (§4.4): the package's launcher icon drawn
     * at size×size, header + raw ARGB8888 pixels. Throws on an unknown package
     * or a drawing failure (the caller answers EINVAL).
     *
     * <p>The icon is loaded from a hand-built {@code Resources} over the app's
     * base APK <em>plus all splits</em>, read at an explicit density: split
     * installs keep the launcher icon bitmaps in a {@code split_config.<dpi>}
     * APK, which the synthetic app_process context's resource loader skips —
     * {@code getApplicationIcon} then falls back to the framework's
     * {@code sym_def_app_icon}, whose load fails too (no display configuration).
     * Anything the manual path can't serve falls through to
     * {@code getApplicationIcon} (fine for monolithic APKs).
     */
    byte[] appIconPayload(String pkg, int size) throws Exception {
        ApplicationInfo ai = pm.getApplicationInfo(pkg, 0);  // NameNotFound -> EINVAL
        Drawable d = null;
        if (ai.icon != 0) {
            try {
                d = splitAwareResources(ai).getDrawableForDensity(
                        ai.icon, android.util.DisplayMetrics.DENSITY_XXHIGH, null);
            } catch (Throwable t) {
                System.err.println("tab5adb-agent: split icon load " + pkg + ": " + t);
                // fall through to the default loader
            }
        }
        if (d == null) d = pm.getApplicationIcon(ai);
        Bitmap bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(bmp);
        d.setBounds(0, 0, size, size);
        d.draw(c);
        int[] px = new int[size * size];
        bmp.getPixels(px, 0, size, 0, 0, size, size);
        bmp.recycle();

        byte[] out = new byte[8 + px.length * 4];
        out[0] = (byte) size;          // width (LE)
        out[1] = (byte) (size >> 8);
        out[2] = (byte) size;          // height (LE)
        out[3] = (byte) (size >> 8);
        out[4] = 0x01;                 // format = ARGB8888
        int o = 8;
        for (int v : px) {  // Color int 0xAARRGGBB, LE -> bytes B,G,R,A
            out[o++] = (byte) v;
            out[o++] = (byte) (v >> 8);
            out[o++] = (byte) (v >> 16);
            out[o++] = (byte) (v >> 24);
        }
        return out;
    }

    /**
     * A {@code Resources} over the app's base APK + every split, assembled the
     * way the framework itself mounts split apps: hidden
     * {@code ApkAssets.loadFromPath} per APK + {@code AssetManager.setApkAssets}
     * (which merges same-package-id tables across splits — the legacy
     * {@code addAssetPath} keeps them as separate packages and density-split
     * values stay unresolvable). The framework-res ApkAssets is included since
     * {@code setApkAssets} replaces the default system set.
     */
    private static android.content.res.Resources splitAwareResources(ApplicationInfo ai)
            throws Exception {
        Class<?> apkAssetsClass = Class.forName("android.content.res.ApkAssets");
        java.lang.reflect.Method load = apkAssetsClass.getMethod("loadFromPath", String.class);
        java.util.ArrayList<Object> assets = new java.util.ArrayList<>();
        assets.add(load.invoke(null, "/system/framework/framework-res.apk"));
        assets.add(load.invoke(null, ai.publicSourceDir));
        if (ai.splitPublicSourceDirs != null) {
            for (String split : ai.splitPublicSourceDirs) assets.add(load.invoke(null, split));
        }
        Object arr = java.lang.reflect.Array.newInstance(apkAssetsClass, assets.size());
        for (int i = 0; i < assets.size(); i++) java.lang.reflect.Array.set(arr, i, assets.get(i));

        android.content.res.AssetManager am =
                android.content.res.AssetManager.class.getDeclaredConstructor().newInstance();
        android.content.res.AssetManager.class
                .getMethod("setApkAssets", arr.getClass(), boolean.class)
                .invoke(am, arr, true);
        android.util.DisplayMetrics dm = new android.util.DisplayMetrics();
        dm.setToDefaults();
        android.content.res.Configuration cfg = new android.content.res.Configuration();
        cfg.setToDefaults();
        return new android.content.res.Resources(am, dm, cfg);
    }

    /** UTF-8 bytes capped to 255, truncated on a character boundary. */
    private static byte[] utf8Capped(String s) {
        byte[] b = s.getBytes(StandardCharsets.UTF_8);
        while (b.length > 255) {
            s = s.substring(0, s.length() - 1);
            b = s.getBytes(StandardCharsets.UTF_8);
        }
        return b;
    }
}
