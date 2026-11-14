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
import java.io.EOFException;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Locale;

final class VideoServer {
    private static final int HEADER_BYTES = 8;
    private static final int VIDEO_START = 0x01;
    private static final int FRAME_ACK = 0xC0;
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
    private static final int FLAG_ADAPT = 0x04;
    private static final int MAX_IN = 1024;
    private static final int DEFAULT_QUALITY = 80;
    private static final int MIN_QUALITY = 10;
    private static final int QUALITY_STEP = 15;
    private static final long IDLE_MS = 5;
    private static final long STATS_NS = 5_000_000_000L;

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

    private static final class Frame {
        final ArrayList<byte[]> strips = new ArrayList<>();
        // Every strip is in, or the rest were dropped.
        boolean done;
        boolean taken;
    }

    private static final class Session {
        private final LocalSocket socket;
        private final Thread thread;
        private final Thread sender;
        private volatile boolean stopped;
        private VideoPacer pacer;
        private VideoRate rate;
        // The frame to send next, replaced by a newer one until the sender takes it.
        private Frame pending;
        private int frames;
        private int statsFrames;
        private int replaced;
        private long statsBytes;
        private long statsAt;

        Session(LocalSocket socket) {
            this.socket = socket;
            this.thread = new Thread(this::run, "video");
            this.sender = new Thread(this::send, "video-send");
        }

        void start() {
            thread.start();
        }

        void stop() {
            halt();
            closeQuietly(socket);
            try {
                thread.join();
            } catch (InterruptedException ignored) {
            }
        }

        private synchronized void halt() {
            stopped = true;
            notifyAll();
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
                halt();
                closeQuietly(socket);
                if (sender.isAlive()) {
                    try {
                        sender.join();
                    } catch (InterruptedException ignored) {
                    }
                }
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
            if (kind != VIDEO_START || length < 8 || length > MAX_IN) {
                throw new IOException("video stream opened with type " + kind);
            }
            byte[] payload = new byte[(int) length];
            in.readFully(payload);
            Server.received();
            ByteBuffer p = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
            int boxW = p.getShort(0) & 0xFFFF;
            int boxH = p.getShort(2) & 0xFFFF;
            boolean swap = (payload[4] & FLAG_SWAP) != 0;
            boolean fill = (payload[4] & FLAG_FILL) != 0;
            boolean adapt = (payload[4] & FLAG_ADAPT) != 0;
            int quality = payload[5] & 0xFF;
            int maxFps = payload[6] & 0xFF;
            int strips = Math.max(1, payload[7] & 0xFF);
            long maxRate = length >= 12 ? p.getInt(8) & 0xFFFFFFFFL : 0;
            if (boxW < 4 || boxH < 4) {
                reply(out, EINVAL);
                return;
            }
            quality = quality == 0 ? DEFAULT_QUALITY : Math.min(quality, 100);
            pacer = new VideoPacer();
            rate = new VideoRate(quality, maxFps, maxRate, adapt);
            reply(out, OK);
            System.out.println("video: box " + boxW + "x" + boxH + (swap ? " swap" : "") + (fill ? " fill" : "") + (adapt ? " adapt" : "")
                    + " q" + quality + " fps " + maxFps + " strips " + strips + (maxRate > 0 ? " max " + maxRate + " B/s" : ""));
            readAcks(in);
            sender.start();
            stream(boxW, boxH, swap, fill, strips);
        }

        private void readAcks(DataInputStream in) {
            Thread reader = new Thread(() -> {
                byte[] head = new byte[HEADER_BYTES];
                try {
                    while (true) {
                        in.readFully(head);
                        long length = ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN).getInt(0) & 0xFFFFFFFFL;
                        if (length > MAX_IN) {
                            throw new IOException("video message of " + length + " bytes");
                        }
                        byte[] payload = new byte[(int) length];
                        in.readFully(payload);
                        Server.received();
                        if ((head[4] & 0xFF) == FRAME_ACK && length >= 4) {
                            long ack = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN).getInt(0) & 0xFFFFFFFFL;
                            synchronized (this) {
                                pacer.acked(ack, System.nanoTime());
                                notifyAll();
                            }
                        }
                    }
                } catch (IOException e) {
                    if (!stopped && !(e instanceof EOFException)) {
                        System.out.println("video: " + e);
                    }
                }
                halt();
            }, "video-ack");
            reader.setDaemon(true);
            reader.start();
        }

        private void stream(int boxW, int boxH, boolean swap, boolean fill, int strips) throws Exception {
            int max = Server.videoMaxMessage();
            ScreenCapture capture = null;
            int rotation = -1;
            Point logical = null;
            int[] shape = null;
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
                    int quality;
                    int fps;
                    synchronized (this) {
                        rate.update(t0, pacer.bytesPerSecond());
                        rate.encoded(t0);
                        quality = rate.quality();
                        fps = rate.fps();
                    }
                    try {
                        encodeFrame(bitmap, rotation, quality, max, strips);
                    } finally {
                        bitmap.recycle();
                    }
                    if (fps > 0) {
                        long left = 1_000_000_000L / fps - (System.nanoTime() - t0);
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

        private void encodeFrame(Bitmap frame, int rotation, int quality, int max, int strips) {
            int width = frame.getWidth();
            int height = frame.getHeight();
            int rows = Math.max(STRIP_ALIGN, height / strips / STRIP_ALIGN * STRIP_ALIGN);
            int count = Math.max(1, Math.min(strips, height / rows));
            Frame f = new Frame();
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
                synchronized (this) {
                    if (message == null) {
                        f.done = true;
                        if (pending == f) {
                            pending = null;
                        }
                        notifyAll();
                        return;
                    }
                    f.strips.add(message);
                    f.done = last;
                    if (!f.taken && (pending == null || f.done) && pending != f) {
                        if (pending != null) {
                            replaced++;
                        }
                        pending = f;
                    }
                    notifyAll();
                }
            }
        }

        private void send() {
            try {
                OutputStream out = socket.getOutputStream();
                while (true) {
                    Frame f;
                    synchronized (this) {
                        while (!stopped && (pending == null || !pacer.mayStart())) {
                            wait();
                        }
                        if (stopped) {
                            return;
                        }
                        f = pending;
                        pending = null;
                        f.taken = true;
                    }
                    int bytes = 0;
                    for (int i = 0; ; i++) {
                        byte[] message;
                        synchronized (this) {
                            while (!stopped && i >= f.strips.size() && !f.done) {
                                wait();
                            }
                            if (stopped) {
                                return;
                            }
                            if (i >= f.strips.size()) {
                                break;
                            }
                            message = f.strips.get(i);
                            // Before the write: its ack can come back before the write returns.
                            pacer.wrote(message.length, System.nanoTime());
                        }
                        out.write(message);
                        out.flush();
                        bytes += message.length;
                    }
                    synchronized (this) {
                        long now = System.nanoTime();
                        rate.sent(bytes, now);
                        frames++;
                        stats(bytes, now);
                    }
                }
            } catch (IOException e) {
                if (!stopped) {
                    System.out.println("video: " + e);
                }
            } catch (InterruptedException ignored) {
            } finally {
                halt();
            }
        }

        private void stats(int bytes, long now) {
            statsFrames++;
            statsBytes += bytes;
            if (statsAt == 0) {
                statsAt = now;
            }
            long span = now - statsAt;
            if (span < STATS_NS) {
                return;
            }
            System.out.println(String.format(Locale.ROOT, "video: %.1f fps (%d replaced), %.2f MB/s, q%d, fps limit %d, link %.2f MB/s, latency %.1f ms",
                    statsFrames * 1e9 / span, replaced, statsBytes * 1e3 / span, rate.quality(), rate.fps(),
                    pacer.bytesPerSecond() / 1e6, pacer.latencyNs() / 1e6));
            statsAt = now;
            statsBytes = 0;
            statsFrames = 0;
            replaced = 0;
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
