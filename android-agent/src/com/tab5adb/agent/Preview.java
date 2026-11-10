/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Matrix;
import android.media.ExifInterface;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

// The PREVIEW requests: the screenshot kept for them, and files drawn small.
final class Preview {
    static final int IMAGE = 1;
    static final int ATTRS = 2;

    private static final int DEFAULT_QUALITY = 80;
    private static final int MIN_QUALITY = 20;
    private static final int QUALITY_STEP = 15;

    private static final int U32 = 0;
    private static final int RATIO = 2;
    private static final int SIGNED_RATIO = 3;
    private static final int TEXT = 4;

    private static final int WIDTH = 0x0001;
    private static final int HEIGHT = 0x0002;
    private static final int MIME = 0x0003;
    private static final int EXIF_MAKE = 0x0100;
    private static final int EXIF_MODEL = 0x0101;
    private static final int EXIF_LENS = 0x0102;
    private static final int EXIF_SOFTWARE = 0x0103;
    private static final int EXIF_TAKEN = 0x0104;
    private static final int EXIF_ISO = 0x0105;
    private static final int EXIF_EXPOSURE = 0x0106;
    private static final int EXIF_F_NUMBER = 0x0107;
    private static final int EXIF_FOCAL = 0x0108;
    private static final int EXIF_FOCAL_35MM = 0x0109;
    private static final int EXIF_BIAS = 0x010A;
    private static final int EXIF_FLASH = 0x010B;
    private static final int MAX_TEXT = 256;
    // Read by the framework's ExifInterface without a public name for it.
    private static final String TAG_LENS_MODEL = "LensModel";

    private final File saved;
    private Bitmap shot;

    Preview(String id) {
        saved = new File("/data/local/tmp/tab5adb-screenshot-" + id + ".png");
    }

    synchronized byte[] screenshot(int boxWidth, int boxHeight, int quality, long max) throws IOException {
        release();
        shot = capture();
        return renderShot(boxWidth, boxHeight, quality, max);
    }

    // null without a screenshot kept.
    synchronized byte[] renderShot(int boxWidth, int boxHeight, int quality, long max) {
        if (shot == null) {
            return null;
        }
        Attrs attrs = new Attrs();
        attrs.u32(WIDTH, shot.getWidth());
        attrs.u32(HEIGHT, shot.getHeight());
        int[] size = fit(shot.getWidth(), shot.getHeight(), boxWidth, boxHeight);
        Bitmap small = Bitmap.createScaledBitmap(shot, size[0], size[1], true);
        return rendered(small, quality, attrs.bytes(), max);
    }

    // The file's size and path; null without a screenshot kept.
    synchronized byte[] save() throws IOException {
        if (shot == null) {
            return null;
        }
        try (FileOutputStream out = new FileOutputStream(saved)) {
            if (!shot.compress(Bitmap.CompressFormat.PNG, 100, out)) {
                throw new IOException("png");
            }
        }
        byte[] path = saved.getPath().getBytes(StandardCharsets.UTF_8);
        ByteBuffer result = ByteBuffer.allocate(4 + path.length).order(ByteOrder.LITTLE_ENDIAN);
        result.putInt((int) saved.length()).put(path);
        return result.array();
    }

    synchronized void release() {
        if (shot != null) {
            shot.recycle();
            shot = null;
        }
        saved.delete();
    }

    // `screencap` without -p writes the pixels after a header of 12 bytes, or
    // 16 from Android 9 on.
    private static Bitmap capture() throws IOException {
        Process process = Runtime.getRuntime().exec(new String[] {"screencap"});
        byte[] raw;
        try (InputStream in = process.getInputStream()) {
            ByteArrayOutputStream out = new ByteArrayOutputStream(16 * 1024 * 1024);
            byte[] buf = new byte[256 * 1024];
            for (int n; (n = in.read(buf)) > 0; ) {
                out.write(buf, 0, n);
            }
            raw = out.toByteArray();
        }
        try {
            process.waitFor();
        } catch (InterruptedException e) {
            throw new IOException("screencap interrupted");
        }
        if (raw.length < 12) {
            throw new IOException("screencap: " + raw.length + " bytes");
        }
        ByteBuffer header = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN);
        int width = header.getInt(0);
        int height = header.getInt(4);
        int format = header.getInt(8);
        long pixels = (long) width * height * 4;
        long head = raw.length - pixels;
        if (width <= 0 || height <= 0 || (format != 1 && format != 2) || (head != 12 && head != 16)) {
            throw new IOException("screencap: " + width + "x" + height + " format " + format + ", " + raw.length + " bytes");
        }
        Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(raw, (int) head, (int) pixels));
        bitmap.setHasAlpha(false);
        return bitmap;
    }

    // null for a kind of file not drawn here.
    byte[] renderFile(String path, int boxWidth, int boxHeight, int quality, int flags, long max) throws IOException {
        File file = new File(path);
        if (!file.canRead()) {
            throw new IOException("cannot read " + path);
        }
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(path, bounds);
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
            return null;
        }
        ExifInterface exif = null;
        try {
            exif = new ExifInterface(path);
        } catch (IOException | RuntimeException e) {
            System.out.println("exif " + path + ": " + e);
        }
        int orientation = exif != null ? exif.getAttributeInt(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL) : ExifInterface.ORIENTATION_NORMAL;
        boolean swap = orientation >= ExifInterface.ORIENTATION_TRANSPOSE && orientation <= ExifInterface.ORIENTATION_ROTATE_270;
        int width = swap ? bounds.outHeight : bounds.outWidth;
        int height = swap ? bounds.outWidth : bounds.outHeight;
        byte[] attrs = new byte[2];
        if ((flags & ATTRS) != 0) {
            Attrs a = new Attrs();
            a.u32(WIDTH, width);
            a.u32(HEIGHT, height);
            if (bounds.outMimeType != null) {
                a.text(MIME, bounds.outMimeType);
            }
            if (exif != null) {
                exifAttrs(exif, a);
            }
            attrs = a.bytes();
        }
        if ((flags & IMAGE) == 0) {
            return rendered(null, quality, attrs, max);
        }
        int[] size = fit(width, height, boxWidth, boxHeight);
        int scaledWidth = swap ? size[1] : size[0];
        int scaledHeight = swap ? size[0] : size[1];
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inSampleSize = 1;
        while (bounds.outWidth / (options.inSampleSize * 2) >= scaledWidth && bounds.outHeight / (options.inSampleSize * 2) >= scaledHeight) {
            options.inSampleSize *= 2;
        }
        Bitmap decoded = BitmapFactory.decodeFile(path, options);
        if (decoded == null) {
            throw new IOException("cannot decode " + path);
        }
        Bitmap scaled = Bitmap.createScaledBitmap(decoded, scaledWidth, scaledHeight, true);
        Matrix turn = turn(orientation);
        Bitmap upright = turn == null ? scaled : Bitmap.createBitmap(scaled, 0, 0, scaledWidth, scaledHeight, turn, true);
        return rendered(upright, quality, attrs, max);
    }

    private static Matrix turn(int orientation) {
        Matrix m = new Matrix();
        switch (orientation) {
            case ExifInterface.ORIENTATION_FLIP_HORIZONTAL:
                m.postScale(-1, 1);
                break;
            case ExifInterface.ORIENTATION_ROTATE_180:
                m.postRotate(180);
                break;
            case ExifInterface.ORIENTATION_FLIP_VERTICAL:
                m.postScale(1, -1);
                break;
            case ExifInterface.ORIENTATION_TRANSPOSE:
                m.postRotate(90);
                m.postScale(-1, 1);
                break;
            case ExifInterface.ORIENTATION_ROTATE_90:
                m.postRotate(90);
                break;
            case ExifInterface.ORIENTATION_TRANSVERSE:
                m.postRotate(-90);
                m.postScale(-1, 1);
                break;
            case ExifInterface.ORIENTATION_ROTATE_270:
                m.postRotate(-90);
                break;
            default:
                return null;
        }
        return m;
    }

    private static void exifAttrs(ExifInterface exif, Attrs a) {
        a.exifText(EXIF_MAKE, exif.getAttribute(ExifInterface.TAG_MAKE));
        a.exifText(EXIF_MODEL, exif.getAttribute(ExifInterface.TAG_MODEL));
        a.exifText(EXIF_LENS, exif.getAttribute(TAG_LENS_MODEL));
        a.exifText(EXIF_SOFTWARE, exif.getAttribute(ExifInterface.TAG_SOFTWARE));
        String taken = exif.getAttribute(ExifInterface.TAG_DATETIME_ORIGINAL);
        a.exifText(EXIF_TAKEN, taken != null ? taken : exif.getAttribute(ExifInterface.TAG_DATETIME));
        int iso = exif.getAttributeInt(ExifInterface.TAG_ISO_SPEED_RATINGS, 0);
        if (iso > 0) {
            a.u32(EXIF_ISO, iso);
        }
        double exposure = exif.getAttributeDouble(ExifInterface.TAG_EXPOSURE_TIME, 0);
        if (exposure > 0) {
            if (exposure < 1) {
                a.ratio(EXIF_EXPOSURE, 1, Math.max(1, Math.round(1 / exposure)));
            } else {
                a.ratio(EXIF_EXPOSURE, Math.round(exposure * 1000), 1000);
            }
        }
        double fNumber = exif.getAttributeDouble(ExifInterface.TAG_F_NUMBER, 0);
        if (fNumber > 0) {
            a.ratio(EXIF_F_NUMBER, Math.round(fNumber * 100), 100);
        }
        double focal = exif.getAttributeDouble(ExifInterface.TAG_FOCAL_LENGTH, 0);
        if (focal > 0) {
            a.ratio(EXIF_FOCAL, Math.round(focal * 100), 100);
        }
        int focal35 = exif.getAttributeInt(ExifInterface.TAG_FOCAL_LENGTH_IN_35MM_FILM, 0);
        if (focal35 > 0) {
            a.u32(EXIF_FOCAL_35MM, focal35);
        }
        if (exif.getAttribute(ExifInterface.TAG_EXPOSURE_BIAS_VALUE) != null) {
            a.signedRatio(EXIF_BIAS, Math.round(exif.getAttributeDouble(ExifInterface.TAG_EXPOSURE_BIAS_VALUE, 0) * 100), 100);
        }
        int flash = exif.getAttributeInt(ExifInterface.TAG_FLASH, -1);
        if (flash >= 0) {
            a.u32(EXIF_FLASH, flash);
        }
    }

    // Contain-fit, never enlarged, in multiples of 4.
    private static int[] fit(int width, int height, int boxWidth, int boxHeight) {
        double scale = Math.min(1.0, Math.min((double) boxWidth / width, (double) boxHeight / height));
        int w = Math.max(4, (int) (width * scale) & ~3);
        int h = Math.max(4, (int) (height * scale) & ~3);
        return new int[] {w, h};
    }

    // RENDERED with `image`, at a lower quality where it does not fit in max.
    private static byte[] rendered(Bitmap image, int quality, byte[] attrs, long max) {
        int q = quality == 0 ? DEFAULT_QUALITY : quality;
        while (true) {
            byte[] jpeg = new byte[0];
            if (image != null) {
                ByteArrayOutputStream out = new ByteArrayOutputStream(64 * 1024);
                image.compress(Bitmap.CompressFormat.JPEG, q, out);
                jpeg = out.toByteArray();
            }
            if (image == null || 8 + jpeg.length + attrs.length <= max || q == MIN_QUALITY) {
                ByteBuffer result = ByteBuffer.allocate(8 + jpeg.length + attrs.length).order(ByteOrder.LITTLE_ENDIAN);
                result.putShort((short) (image != null ? image.getWidth() : 0));
                result.putShort((short) (image != null ? image.getHeight() : 0));
                result.putInt(jpeg.length).put(jpeg).put(attrs);
                return result.array();
            }
            q = Math.max(MIN_QUALITY, q - QUALITY_STEP);
        }
    }

    private static final class Attrs {
        private final ByteArrayOutputStream out = new ByteArrayOutputStream();
        private int count;

        void u32(int key, long value) {
            ByteBuffer v = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt((int) value);
            put(key, U32, v.array());
        }

        void ratio(int key, long num, long den) {
            ByteBuffer v = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putInt((int) num).putInt((int) den);
            put(key, RATIO, v.array());
        }

        void signedRatio(int key, long num, long den) {
            ByteBuffer v = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putInt((int) num).putInt((int) den);
            put(key, SIGNED_RATIO, v.array());
        }

        void text(int key, String value) {
            put(key, TEXT, value.getBytes(StandardCharsets.UTF_8));
        }

        void exifText(int key, String value) {
            if (value == null) {
                return;
            }
            String trimmed = value.trim();
            if (!trimmed.isEmpty()) {
                text(key, trimmed.length() > MAX_TEXT ? trimmed.substring(0, MAX_TEXT) : trimmed);
            }
        }

        private void put(int key, int type, byte[] value) {
            ByteBuffer head = ByteBuffer.allocate(6).order(ByteOrder.LITTLE_ENDIAN);
            head.putShort((short) key).put((byte) type).put((byte) 0).putShort((short) value.length);
            out.write(head.array(), 0, 6);
            out.write(value, 0, value.length);
            count++;
        }

        byte[] bytes() {
            byte[] body = out.toByteArray();
            ByteBuffer all = ByteBuffer.allocate(2 + body.length).order(ByteOrder.LITTLE_ENDIAN);
            all.putShort((short) count).put(body);
            return all.array();
        }
    }
}
