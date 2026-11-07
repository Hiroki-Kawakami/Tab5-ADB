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
    private static final int REPLY = 0x40;
    private static final int FRAME = 0x41;
    private static final int FRAME_HEAD = 8;
    private static final int OK = 0x00;
    private static final int EINVAL = 0x01;
    private static final int CODEC_JPEG = 1;
    private static final int FLAG_SWAP = 0x01;
    private static final int DEFAULT_QUALITY = 80;
    private static final int MIN_QUALITY = 10;
    private static final int QUALITY_STEP = 15;
    private static final long IDLE_MS = 5;

    private static Session current;

    private VideoServer() {
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
            int quality = payload[5] & 0xFF;
            int maxFps = payload[6] & 0xFF;
            if (boxW < 4 || boxH < 4) {
                reply(out, EINVAL);
                return;
            }
            quality = quality == 0 ? DEFAULT_QUALITY : Math.min(quality, 100);
            reply(out, OK);
            System.out.println("video: box " + boxW + "x" + boxH + (swap ? " swap" : "") + " q" + quality + " fps " + maxFps);
            watchClose(in);
            stream(out, boxW, boxH, swap, quality, maxFps);
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

        private void stream(OutputStream out, int boxW, int boxH, boolean swap, int quality, int maxFps) throws Exception {
            long frameNs = maxFps > 0 ? 1_000_000_000L / maxFps : 0;
            int max = Server.videoMaxMessage();
            ScreenCapture capture = null;
            int rotation = -1;
            Point logical = null;
            int frames = 0;
            long dbgStart = System.nanoTime(), dbgProbe = 0, dbgAcq = 0, dbgEnc = 0, dbgWrite = 0, dbgBytes = 0, dbgIdle = 0;
            int dbgFrames = 0;
            try {
                while (!stopped) {
                    long t0 = System.nanoTime();
                    if (t0 - dbgStart >= 1_000_000_000L && dbgFrames > 0) {
                        System.out.println("DBG agent " + dbgFrames + " fps probe " + dbgProbe / dbgFrames / 1000 + "us acq " + dbgAcq / dbgFrames / 1000 + "us enc " + dbgEnc / dbgFrames / 1000 + "us write " + dbgWrite / dbgFrames / 1000 + "us idle " + dbgIdle / 1_000_000 + "ms " + dbgBytes / dbgFrames / 1024 + "KB");
                        dbgStart = t0; dbgProbe = dbgAcq = dbgEnc = dbgWrite = dbgBytes = dbgIdle = 0; dbgFrames = 0;
                    }
                    int now = ScreenCapture.rotation();
                    Point size = ScreenCapture.logicalSize();
                    if (capture == null || now != rotation || !size.equals(logical)) {
                        if (capture != null) {
                            capture.close();
                        }
                        rotation = now;
                        logical = size;
                        boolean turned = swap && (rotation & 1) != 0;
                        int[] fit = fit(size.x, size.y, turned ? boxH : boxW, turned ? boxW : boxH);
                        capture = new ScreenCapture(fit[0], fit[1], size.x, size.y);
                        System.out.println("video: " + fit[0] + "x" + fit[1] + " rotation " + rotation);
                    }
                    long t1 = System.nanoTime();
                    Bitmap bitmap = capture.acquire();
                    long t2 = System.nanoTime();
                    if (bitmap == null) {
                        Thread.sleep(IDLE_MS);
                        dbgIdle += System.nanoTime() - t1;
                        continue;
                    }
                    byte[] message;
                    try {
                        message = encode(bitmap, rotation, quality, max);
                    } finally {
                        bitmap.recycle();
                    }
                    long t3 = System.nanoTime();
                    if (message != null) {
                        out.write(message);
                        out.flush();
                        frames++;
                    }
                    long t4 = System.nanoTime();
                    dbgProbe += t1 - t0; dbgAcq += t2 - t1; dbgEnc += t3 - t2; dbgWrite += t4 - t3; dbgFrames++;
                    dbgBytes += message != null ? message.length : 0;
                    if (frameNs > 0) {
                        long left = frameNs - (System.nanoTime() - t0);
                        if (left > 0) {
                            Thread.sleep(left / 1_000_000L, (int) (left % 1_000_000L));
                        }
                    }
                }
            } finally {
                if (capture != null) {
                    capture.close();
                }
                System.out.println("video: ended after " + frames + " frames");
            }
        }
    }

    static int[] fit(int srcW, int srcH, int boxW, int boxH) {
        double s = Math.min((double) boxW / srcW, (double) boxH / srcH);
        int w = Math.max(4, Math.min(boxW, (int) (srcW * s)) & ~3);
        int h = Math.max(4, Math.min(boxH, (int) (srcH * s)) & ~3);
        return new int[]{w, h};
    }

    // null when even the lowest quality does not fit in max.
    private static byte[] encode(Bitmap bitmap, int rotation, int quality, int max) {
        for (int q = quality; ; q = Math.max(MIN_QUALITY, q - QUALITY_STEP)) {
            ByteArrayOutputStream jpeg = new ByteArrayOutputStream(64 * 1024);
            bitmap.compress(Bitmap.CompressFormat.JPEG, q, jpeg);
            int length = FRAME_HEAD + jpeg.size();
            if (length <= max) {
                ByteBuffer m = ByteBuffer.allocate(HEADER_BYTES + length).order(ByteOrder.LITTLE_ENDIAN);
                m.putInt(length).put((byte) FRAME).put((byte) 0).putShort((short) 0);
                m.putShort((short) bitmap.getWidth()).putShort((short) bitmap.getHeight());
                m.put((byte) rotation).put((byte) 0).putShort((short) 0);
                m.put(jpeg.toByteArray());
                return m.array();
            }
            if (q == MIN_QUALITY) {
                System.out.println("video: frame of " + length + " bytes dropped");
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
