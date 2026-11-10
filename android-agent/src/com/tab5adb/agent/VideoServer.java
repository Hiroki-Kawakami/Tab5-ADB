/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import android.graphics.Bitmap;
import android.graphics.Point;
import android.net.LocalServerSocket;
import android.net.LocalSocket;

import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

final class VideoServer {
    private static final int HEADER_BYTES = 8;
    private static final int VIDEO_START = 0x01;
    private static final int REPLY = 0x80;
    private static final int FRAME = 0x81;
    private static final int FRAME_HEAD = 12;
    private static final int FLAG_LAST = 0x01;
    private static final int STRIP_ALIGN = 16;
    private static final int OK = 0x00;
    private static final int EINVAL = 0x01;
    private static final int CODEC_JPEG = 1;
    private static final int FLAG_SWAP = 0x01;
    private static final int FLAG_FILL = 0x02;
    private static final int DEFAULT_QUALITY = 80;
    private static final int MIN_QUALITY = 10;
    private static final int QUALITY_STEP = 15;
    private static final long IDLE_MS = 5;

    private static Session current;
    private static volatile Sent sent;

    static final class Sent {
        final int width;
        final int height;
        final int rotation;
        // Where the frame lies in the captured picture of captureWidth x captureHeight.
        final int cropX;
        final int cropY;
        final int captureWidth;
        final int captureHeight;
        final int logicalWidth;
        final int logicalHeight;

        Sent(int[] shape, int rotation, Point logical) {
            this.width = shape[0];
            this.height = shape[1];
            this.captureWidth = shape[2];
            this.captureHeight = shape[3];
            this.cropX = shape[4];
            this.cropY = shape[5];
            this.rotation = rotation;
            this.logicalWidth = logical.x;
            this.logicalHeight = logical.y;
        }
    }

    private VideoServer() {
    }

    static Sent sent() {
        return sent;
    }

    static void listen(LocalServerSocket server) {
        Thread thread = new Thread(() -> {
            while (true) {
                LocalSocket socket;
                try {
                    socket = server.accept();
                } catch (IOException e) {
                    return;
                }
                Session previous;
                Session next = Server.helloDone() ? new Session(socket) : null;
                synchronized (VideoServer.class) {
                    previous = current;
                    current = next;
                }
                if (previous != null) {
                    previous.stop();
                }
                if (next == null) {
                    closeQuietly(socket);
                } else {
                    next.start();
                }
            }
        }, "video-accept");
        thread.setDaemon(true);
        thread.start();
    }

    private static void closeQuietly(LocalSocket socket) {
        try {
            socket.close();
        } catch (IOException ignored) {
        }
    }

    private static final class Session {
        private final LocalSocket socket;
        private final Thread thread;
        private volatile boolean stopped;

        Session(LocalSocket socket) {
            this.socket = socket;
            this.thread = new Thread(this::run, "video");
        }

        void start() {
            thread.start();
        }

        void stop() {
            stopped = true;
            closeQuietly(socket);
            try {
                thread.join();
            } catch (InterruptedException ignored) {
            }
        }

        private void run() {
            try {
                serve();
            } catch (IOException e) {
                if (!stopped) {
                    System.out.println("video: " + e);
                }
            } catch (Exception e) {
                System.out.println("video: " + e);
            } finally {
                stopped = true;
                closeQuietly(socket);
            }
        }

        private void serve() throws Exception {
            DataInputStream in = new DataInputStream(socket.getInputStream());
            OutputStream out = socket.getOutputStream();
            byte[] head = new byte[HEADER_BYTES];
            in.readFully(head);
            ByteBuffer h = ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN);
            long length = h.getInt(0) & 0xFFFFFFFFL;
            int kind = head[4] & 0xFF;
            if (kind != VIDEO_START || length < 8 || length > 1024) {
                throw new IOException("video stream opened with type " + kind);
            }
            byte[] payload = new byte[(int) length];
            in.readFully(payload);
            ByteBuffer p = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
            int boxW = p.getShort(0) & 0xFFFF;
            int boxH = p.getShort(2) & 0xFFFF;
            boolean swap = (payload[4] & FLAG_SWAP) != 0;
            boolean fill = (payload[4] & FLAG_FILL) != 0;
            int quality = payload[5] & 0xFF;
            int maxFps = payload[6] & 0xFF;
            int strips = Math.max(1, payload[7] & 0xFF);
            if (boxW < 4 || boxH < 4) {
                reply(out, EINVAL);
                return;
            }
            quality = quality == 0 ? DEFAULT_QUALITY : Math.min(quality, 100);
            reply(out, OK);
            System.out.println("video: box " + boxW + "x" + boxH + (swap ? " swap" : "") + (fill ? " fill" : "") + " q" + quality + " fps " + maxFps + " strips " + strips);
            watchClose(in);
            stream(out, boxW, boxH, swap, fill, quality, maxFps, strips);
        }

        private void watchClose(InputStream in) {
            Thread thread = new Thread(() -> {
                try {
                    while (in.read() >= 0) {
                    }
                } catch (IOException ignored) {
                }
                stopped = true;
            }, "video-watch");
            thread.setDaemon(true);
            thread.start();
        }

        private void stream(OutputStream out, int boxW, int boxH, boolean swap, boolean fill, int quality, int maxFps, int strips) throws Exception {
            long frameNs = maxFps > 0 ? 1_000_000_000L / maxFps : 0;
            int max = Server.videoMaxMessage();
            ScreenCapture capture = null;
            int rotation = -1;
            Point logical = null;
            int[] shape = null;
            int frames = 0;
            try {
                while (!stopped) {
                    long t0 = System.nanoTime();
                    int now = ScreenCapture.rotation();
                    Point size = ScreenCapture.logicalSize();
                    if (capture == null || now != rotation || !size.equals(logical)) {
                        if (capture != null) {
                            capture.close();
                        }
                        rotation = now;
                        logical = size;
                        boolean turned = swap && (rotation & 1) != 0;
                        shape = shape(size.x, size.y, turned ? boxH : boxW, turned ? boxW : boxH, fill);
                        capture = new ScreenCapture(shape[2], shape[3], size.x, size.y);
                        sent = new Sent(shape, rotation, size);
                        System.out.println("video: " + shape[0] + "x" + shape[1] + " of " + shape[2] + "x" + shape[3] + " rotation " + rotation);
                    }
                    Bitmap bitmap = capture.acquire();
                    if (bitmap == null) {
                        Thread.sleep(IDLE_MS);
                        continue;
                    }
                    if (shape[0] != shape[2] || shape[1] != shape[3]) {
                        Bitmap cropped = Bitmap.createBitmap(bitmap, shape[4], shape[5], shape[0], shape[1]);
                        bitmap.recycle();
                        bitmap = cropped;
                    }
                    try {
                        sendFrame(out, bitmap, rotation, quality, max, strips);
                    } finally {
                        bitmap.recycle();
                    }
                    frames++;
                    if (frameNs > 0) {
                        long left = frameNs - (System.nanoTime() - t0);
                        if (left > 0) {
                            Thread.sleep(left / 1_000_000L, (int) (left % 1_000_000L));
                        }
                    }
                }
            } finally {
                sent = null;
                if (capture != null) {
                    capture.close();
                }
                System.out.println("video: ended after " + frames + " frames");
            }
        }
    }

    // {frame w, h, capture w, h, crop x, y}: fitted in the box, or covering it
    // and cropped to it.
    static int[] shape(int srcW, int srcH, int boxW, int boxH, boolean fill) {
        if (!fill) {
            double s = Math.min((double) boxW / srcW, (double) boxH / srcH);
            int w = Math.max(4, Math.min(boxW, (int) (srcW * s)) & ~3);
            int h = Math.max(4, Math.min(boxH, (int) (srcH * s)) & ~3);
            return new int[]{w, h, w, h, 0, 0};
        }
        int w = Math.max(4, boxW & ~3);
        int h = Math.max(4, boxH & ~3);
        double s = Math.max((double) w / srcW, (double) h / srcH);
        int cw = Math.max(w, (int) Math.ceil(srcW * s));
        int ch = Math.max(h, (int) Math.ceil(srcH * s));
        return new int[]{w, h, cw, ch, (cw - w) / 2, (ch - h) / 2};
    }

    private static void sendFrame(OutputStream out, Bitmap frame, int rotation, int quality, int max, int strips) throws IOException {
        int width = frame.getWidth();
        int height = frame.getHeight();
        int rows = Math.max(STRIP_ALIGN, height / strips / STRIP_ALIGN * STRIP_ALIGN);
        int count = Math.max(1, Math.min(strips, height / rows));
        for (int i = 0; i < count; i++) {
            int y = i * rows;
            boolean last = i == count - 1;
            int band = last ? height - y : rows;
            Bitmap strip = band == height ? frame : Bitmap.createBitmap(frame, 0, y, width, band);
            byte[] message;
            try {
                message = encode(strip, width, height, y, rotation, last, quality, max);
            } finally {
                if (strip != frame) {
                    strip.recycle();
                }
            }
            if (message == null) {
                return;
            }
            out.write(message);
            out.flush();
        }
    }

    // null when even the lowest quality does not fit in max.
    private static byte[] encode(Bitmap strip, int width, int height, int y, int rotation, boolean last, int quality, int max) {
        for (int q = quality; ; q = Math.max(MIN_QUALITY, q - QUALITY_STEP)) {
            ByteArrayOutputStream jpeg = new ByteArrayOutputStream(32 * 1024);
            strip.compress(Bitmap.CompressFormat.JPEG, q, jpeg);
            int length = FRAME_HEAD + jpeg.size();
            if (length <= max) {
                ByteBuffer m = ByteBuffer.allocate(HEADER_BYTES + length).order(ByteOrder.LITTLE_ENDIAN);
                m.putInt(length).put((byte) FRAME).put((byte) 0).putShort((short) 0);
                m.putShort((short) width).putShort((short) height);
                m.putShort((short) y).putShort((short) strip.getHeight());
                m.put((byte) rotation).put((byte) (last ? FLAG_LAST : 0)).putShort((short) 0);
                m.put(jpeg.toByteArray());
                return m.array();
            }
            if (q == MIN_QUALITY) {
                System.out.println("video: strip of " + length + " bytes dropped");
                return null;
            }
        }
    }

    private static void reply(OutputStream out, int status) throws IOException {
        ByteBuffer m = ByteBuffer.allocate(HEADER_BYTES + 8).order(ByteOrder.LITTLE_ENDIAN);
        m.putInt(8).put((byte) REPLY).put((byte) 0).putShort((short) 0);
        m.put((byte) status).put((byte) 0).putShort((short) 0);
        m.put((byte) (status == OK ? CODEC_JPEG : 0)).put((byte) 0).putShort((short) 0);
        out.write(m.array());
        out.flush();
    }
}
