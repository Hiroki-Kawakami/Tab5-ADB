/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.Looper;
import android.os.SystemClock;

import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class Server {
    private static final int VERSION_MAJOR = 0;
    private static final int VERSION_MINOR = 3;
    private static final int VERSION_PATCH = 0;

    private static final int PROTO_VERSION = 1;
    private static final int HEADER_BYTES = 8;
    private static final int MAX_MESSAGE = 64 * 1024;
    private static final long CONNECT_TIMEOUT_MS = 10_000;
    private static final long SILENCE_TIMEOUT_MS = 5_000;
    private static final long SILENCE_CHECK_MS = 250;

    private static final int HELLO = 0x01;
    private static final int SET_ROTATION = 0x10;
    private static final int ADAPT_DISPLAY = 0x11;
    private static final int GET_APP_INFO = 0x20;
    private static final int GET_MEDIA = 0x30;
    private static final int GET_MEDIA_RENDER = 0x31;
    private static final int MEDIA_CONTROL = 0x32;
    private static final int SCREENSHOT = 0x40;
    private static final int SCREENSHOT_RENDER = 0x41;
    private static final int SCREENSHOT_SAVE = 0x42;
    private static final int SCREENSHOT_RELEASE = 0x43;
    private static final int RENDER_FILE = 0x44;
    private static final int MEDIA = 0xB0;
    private static final int REQUEST_LAST = 0x7F;
    private static final int REPLY = 0x80;
    private static final int TOUCH = 0xC0;
    private static final int KEY = 0xC1;
    private static final int INPUT_CLEAR = 0xC2;
    private static final int KEY_BYTES = 4;
    private static final int TOUCH_HEAD = 12;
    private static final int TOUCH_POINT = 6;
    private static final int MAX_POINTERS = 10;

    private static final int OK = 0x00;
    private static final int EINVAL = 0x01;
    private static final int ENOTSUP = 0x02;
    private static final int EBUSY = 0x03;
    private static final int ESTATE = 0x04;
    private static final int EFAIL = 0xFF;

    private static final int CAP_VIDEO = 1 << 0;
    private static final int CAP_AUDIO = 1 << 1;
    private static final int CAP_INPUT = 1 << 2;
    private static final int CAP_MEDIA = 1 << 3;
    private static final int CAP_PREVIEW = 1 << 4;
    private static final int CAP_APPS = 1 << 5;
    private static final int ROTATION_RELEASE = 0xFF;

    private static volatile boolean helloDone;
    private static volatile int videoMaxMessage;
    private static volatile long receivedAt;
    private static volatile Input input;
    private static boolean inputFailed;
    private static AppInfo apps;
    private static MediaInfo media;
    private static Preview preview;

    private final OutputStream out;
    private boolean hello;
    private volatile long replyMax = 0xFFFFFFFFL;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final ExecutorService previews = Executors.newSingleThreadExecutor();
    private final Object notices = new Object();
    private byte[] mediaNotice;
    private final int[] touchIds = new int[MAX_POINTERS];
    private final float[] touchXs = new float[MAX_POINTERS];
    private final float[] touchYs = new float[MAX_POINTERS];

    private Server(OutputStream out) {
        this.out = out;
    }

    public static void main(String[] args) {
        if (args.length != 1) {
            System.out.println("usage: Server <id>");
            System.exit(2);
        }
        String name = "tab5adb-agent-" + args[0];
        LocalServerSocket control;
        try {
            control = new LocalServerSocket(name);
            VideoServer.listen(new LocalServerSocket(name + "-video"));
            AudioServer.listen(new LocalServerSocket(name + "-audio"));
        } catch (IOException e) {
            System.out.println("listen: " + e);
            System.exit(1);
            return;
        }
        Thread watchdog = new Thread(() -> {
            try {
                Thread.sleep(CONNECT_TIMEOUT_MS);
            } catch (InterruptedException e) {
                return;
            }
            System.out.println("no connection");
            System.exit(0);
        });
        watchdog.setDaemon(true);
        watchdog.start();
        Looper.prepareMainLooper();
        try {
            SystemContext.get();
        } catch (Throwable t) {
            System.out.println("context: " + t);
        }
        TextRender text = null;
        try {
            text = TextRender.create();
        } catch (Throwable t) {
            System.out.println("text: " + t);
        }
        if (text != null) {
            try {
                apps = AppInfo.create(text);
            } catch (Throwable t) {
                System.out.println("apps: " + t);
            }
            try {
                media = MediaInfo.create(text);
            } catch (Throwable t) {
                System.out.println("media: " + t);
            }
        }
        preview = new Preview(args[0]);
        DisplayClaim.recover(args[0]);
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            if (input != null) {
                input.clear();
            }
            RotationLock.release();
            DisplaySize.release();
            DisplayClaim.save();
            preview.release();
        }));
        System.out.println("listening");
        System.out.flush();
        try (LocalSocket socket = control.accept()) {
            watchdog.interrupt();
            control.close();
            new Server(socket.getOutputStream()).serve(new DataInputStream(socket.getInputStream()));
        } catch (EOFException e) {
            System.out.println("closed");
        } catch (IOException e) {
            System.out.println("control: " + e);
        }
        System.exit(0);
    }

    static boolean helloDone() {
        return helloDone;
    }

    static int videoMaxMessage() {
        return videoMaxMessage;
    }

    static void received() {
        receivedAt = SystemClock.uptimeMillis();
    }

    // Ends the process once no stream has brought anything for a while; a
    // dropped TCP link closes nothing on this side.
    private static void watchSilence() {
        received();
        Thread thread = new Thread(() -> {
            try {
                while (SystemClock.uptimeMillis() - receivedAt < SILENCE_TIMEOUT_MS) {
                    Thread.sleep(SILENCE_CHECK_MS);
                }
            } catch (InterruptedException e) {
                return;
            }
            System.out.println("silent for " + SILENCE_TIMEOUT_MS + " ms");
            System.exit(0);
        }, "silence");
        thread.setDaemon(true);
        thread.start();
    }

    private void serve(DataInputStream in) throws IOException {
        byte[] head = new byte[HEADER_BYTES];
        while (true) {
            in.readFully(head);
            ByteBuffer h = ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN);
            long length = h.getInt(0) & 0xFFFFFFFFL;
            int kind = head[4] & 0xFF;
            int id = h.getShort(6) & 0xFFFF;
            if (length > MAX_MESSAGE) {
                throw new IOException("message of " + length + " bytes");
            }
            byte[] payload = new byte[(int) length];
            in.readFully(payload);
            received();
            if (!hello && kind != HELLO) {
                throw new IOException("type " + kind + " before HELLO");
            }
            if (kind == HELLO) {
                onHello(id, payload);
            } else if (kind == SET_ROTATION) {
                onSetRotation(id, payload);
            } else if (kind == ADAPT_DISPLAY) {
                onAdaptDisplay(id, payload);
            } else if (kind == GET_APP_INFO && apps != null) {
                worker.execute(() -> onGetAppInfo(id, payload));
            } else if ((kind == GET_MEDIA || kind == GET_MEDIA_RENDER || kind == MEDIA_CONTROL) && media != null) {
                worker.execute(() -> onMedia(kind, id, payload));
            } else if (kind >= SCREENSHOT && kind <= RENDER_FILE) {
                previews.execute(() -> onPreview(kind, id, payload));
            } else if (kind == TOUCH) {
                onTouch(payload);
            } else if (kind == KEY) {
                onKey(payload);
            } else if (kind == INPUT_CLEAR) {
                if (input != null) {
                    input.clear();
                }
            } else if (kind <= REQUEST_LAST) {
                reply(id, ENOTSUP, new byte[0]);
            }
        }
    }

    private void onHello(int id, byte[] payload) throws IOException {
        int proto = payload.length >= 2 ? ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN).getShort(0) & 0xFFFF : -1;
        if (proto != PROTO_VERSION) {
            reply(id, ENOTSUP, new byte[0]);
            throw new IOException("proto " + proto);
        }
        ByteBuffer result = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
        result.putShort((short) PROTO_VERSION);
        result.put((byte) VERSION_MAJOR).put((byte) VERSION_MINOR).put((byte) VERSION_PATCH);
        result.put((byte) 0).putShort((short) 0);
        result.putInt(CAP_VIDEO | (AudioServer.supported() ? CAP_AUDIO : 0) | CAP_INPUT | (media != null ? CAP_MEDIA : 0) | CAP_PREVIEW | (apps != null ? CAP_APPS : 0));
        result.putInt(MAX_MESSAGE);
        if (payload.length >= 12) {
            replyMax = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN).getInt(8) & 0xFFFFFFFFL;
        }
        videoMaxMessage = payload.length >= 16 ? ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN).getInt(12) : 0;
        reply(id, OK, result.array());
        hello = true;
        helloDone = true;
        watchSilence();
        if (media != null) {
            startNotices();
            media.setSink(this::sendMedia);
        }
        System.out.println("HELLO ok");
        System.out.flush();
    }

    private void onSetRotation(int id, byte[] payload) throws IOException {
        int rotation = payload.length >= 1 ? payload[0] & 0xFF : -1;
        if (rotation == ROTATION_RELEASE) {
            RotationLock.release();
            DisplayClaim.save();
            reply(id, OK, new byte[0]);
            return;
        }
        if (rotation < 0 || rotation > 3) {
            reply(id, EINVAL, new byte[0]);
            return;
        }
        int status;
        try {
            if (DisplayClaim.claim()) {
                RotationLock.hold(rotation);
                status = OK;
            } else {
                status = EBUSY;
            }
        } catch (Throwable t) {
            System.out.println("rotation: " + t);
            status = EFAIL;
        }
        DisplayClaim.save();
        reply(id, status, new byte[0]);
    }

    private void onAdaptDisplay(int id, byte[] payload) throws IOException {
        if (payload.length < 4) {
            reply(id, EINVAL, new byte[0]);
            return;
        }
        ByteBuffer p = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        int width = p.getShort(0) & 0xFFFF;
        int height = p.getShort(2) & 0xFFFF;
        if (width == 0 && height == 0) {
            DisplaySize.release();
            DisplayClaim.save();
            reply(id, OK, new byte[0]);
            return;
        }
        if (width == 0 || height == 0) {
            reply(id, EINVAL, new byte[0]);
            return;
        }
        int status;
        try {
            if (!DisplayClaim.claim()) {
                status = EBUSY;
            } else {
                status = DisplaySize.adapt(width, height) ? OK : ESTATE;
            }
        } catch (Throwable t) {
            System.out.println("adapt display: " + t);
            status = EFAIL;
        }
        DisplayClaim.save();
        reply(id, status, new byte[0]);
    }

    private void onGetAppInfo(int id, byte[] payload) {
        try {
            if (payload.length < 4 || (payload[3] & 0xFF) == 0) {
                reply(id, EINVAL, new byte[0]);
                return;
            }
            int iconPx = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN).getShort(0) & 0xFFFF;
            int textPx = payload[2] & 0xFF;
            int count = payload[3] & 0xFF;
            String[] names = new String[count];
            int at = 4;
            for (int i = 0; i < count; i++) {
                int length = at < payload.length ? payload[at] & 0xFF : -1;
                if (length < 0 || at + 1 + length > payload.length) {
                    reply(id, EINVAL, new byte[0]);
                    return;
                }
                names[i] = new String(payload, at + 1, length, StandardCharsets.UTF_8);
                at += 1 + length;
            }
            ByteArrayOutputStream result = new ByteArrayOutputStream();
            for (String name : names) {
                apps.write(result, name, iconPx, textPx);
            }
            reply(id, OK, result.toByteArray());
        } catch (IOException e) {
            System.out.println("app info: " + e);
        }
    }

    private void onMedia(int kind, int id, byte[] payload) {
        try {
            ByteBuffer p = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
            if (kind == GET_MEDIA) {
                reply(id, OK, media.state());
            } else if (kind == GET_MEDIA_RENDER) {
                if (payload.length < 5) {
                    reply(id, EINVAL, new byte[0]);
                    return;
                }
                reply(id, OK, media.render(payload[0] & 0xFF, payload[1] & 0xFF, p.getShort(2) & 0xFFFF, payload[4] & 0xFF));
            } else {
                if (payload.length < 8) {
                    reply(id, EINVAL, new byte[0]);
                    return;
                }
                reply(id, media.control(payload[0] & 0xFF, p.getInt(4) & 0xFFFFFFFFL) ? OK : ESTATE, new byte[0]);
            }
        } catch (Throwable t) {
            System.out.println("media " + kind + ": " + t);
            try {
                reply(id, EFAIL, new byte[0]);
            } catch (IOException e) {
                System.out.println("media " + kind + ": " + e);
            }
        }
    }

    private void onPreview(int kind, int id, byte[] payload) {
        try {
            ByteBuffer p = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
            long max = replyMax - 4;
            byte[] result;
            if (kind == SCREENSHOT_SAVE) {
                result = preview.save();
            } else if (kind == SCREENSHOT_RELEASE) {
                preview.release();
                result = new byte[0];
            } else if (payload.length < (kind == RENDER_FILE ? 9 : 5)) {
                reply(id, EINVAL, new byte[0]);
                return;
            } else {
                int boxWidth = p.getShort(0) & 0xFFFF;
                int boxHeight = p.getShort(2) & 0xFFFF;
                int quality = payload[4] & 0xFF;
                if (kind == SCREENSHOT) {
                    result = preview.screenshot(boxWidth, boxHeight, quality, max);
                } else if (kind == SCREENSHOT_RENDER) {
                    result = preview.renderShot(boxWidth, boxHeight, quality, max);
                } else {
                    String path = new String(payload, 8, payload.length - 8, StandardCharsets.UTF_8);
                    result = preview.renderFile(path, boxWidth, boxHeight, quality, payload[5] & 0xFF, max);
                    if (result == null) {
                        reply(id, ENOTSUP, new byte[0]);
                        return;
                    }
                }
            }
            reply(id, result != null ? OK : ESTATE, result != null ? result : new byte[0]);
        } catch (Throwable t) {
            System.out.println("preview " + kind + ": " + t);
            try {
                reply(id, EFAIL, new byte[0]);
            } catch (IOException e) {
                System.out.println("preview " + kind + ": " + e);
            }
        }
    }

    private void sendMedia(byte[] state) {
        synchronized (notices) {
            mediaNotice = state;
            notices.notify();
        }
    }

    // Notices wait here, so a stalled stream keeps only the newest of each type.
    private void startNotices() {
        Thread thread = new Thread(() -> {
            try {
                while (true) {
                    byte[] state;
                    synchronized (notices) {
                        while (mediaNotice == null) {
                            notices.wait();
                        }
                        state = mediaNotice;
                        mediaNotice = null;
                    }
                    write(MEDIA, 0, state);
                }
            } catch (InterruptedException | IOException e) {
                System.out.println("notices: " + e);
            }
        }, "notices");
        thread.setDaemon(true);
        thread.start();
    }

    private void onTouch(byte[] payload) {
        if (payload.length < TOUCH_HEAD) {
            return;
        }
        ByteBuffer p = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        int n = Math.min(payload[9] & 0xFF, MAX_POINTERS);
        if (payload.length < TOUCH_HEAD + n * TOUCH_POINT || !openInput()) {
            return;
        }
        int width = p.getShort(4) & 0xFFFF;
        int height = p.getShort(6) & 0xFFFF;
        int rotation = payload[8] & 0xFF;
        VideoServer.Sent sent = VideoServer.sent();
        if (sent == null || sent.width != width || sent.height != height || sent.rotation != rotation) {
            input.cancel();
            return;
        }
        float sx = (float) sent.logicalWidth / sent.captureWidth;
        float sy = (float) sent.logicalHeight / sent.captureHeight;
        for (int i = 0; i < n; i++) {
            int at = TOUCH_HEAD + i * TOUCH_POINT;
            touchIds[i] = payload[at] & 0xFF;
            touchXs[i] = ((p.getShort(at + 2) & 0xFFFF) + sent.cropX) * sx;
            touchYs[i] = ((p.getShort(at + 4) & 0xFFFF) + sent.cropY) * sy;
        }
        input.touch(p.getInt(0) & 0xFFFFFFFFL, n, touchIds, touchXs, touchYs);
    }

    private void onKey(byte[] payload) {
        if (payload.length < KEY_BYTES || !openInput()) {
            return;
        }
        int code = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN).getShort(0) & 0xFFFF;
        input.key(code, payload[2] == 0);
    }

    private boolean openInput() {
        if (input == null && !inputFailed) {
            try {
                input = Input.create();
            } catch (Throwable t) {
                inputFailed = true;
                System.out.println("input: " + t);
            }
        }
        return input != null;
    }

    private void reply(int id, int status, byte[] result) throws IOException {
        if (4 + result.length > replyMax) {
            System.out.println("reply of " + (4 + result.length) + " bytes");
            status = EFAIL;
            result = new byte[0];
        }
        byte[] payload = new byte[4 + result.length];
        payload[0] = (byte) status;
        System.arraycopy(result, 0, payload, 4, result.length);
        write(REPLY, id, payload);
    }

    private void write(int kind, int id, byte[] payload) throws IOException {
        ByteBuffer m = ByteBuffer.allocate(HEADER_BYTES + payload.length).order(ByteOrder.LITTLE_ENDIAN);
        m.putInt(payload.length).put((byte) kind).put((byte) 0).putShort((short) id);
        m.put(payload);
        synchronized (out) {
            out.write(m.array());
            out.flush();
        }
    }
}
