/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import android.net.LocalServerSocket;
import android.net.LocalSocket;

import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public final class Server {
    private static final int VERSION_MAJOR = 0;
    private static final int VERSION_MINOR = 2;
    private static final int VERSION_PATCH = 0;

    private static final int PROTO_VERSION = 1;
    private static final int HEADER_BYTES = 8;
    private static final int MAX_MESSAGE = 64 * 1024;
    private static final long CONNECT_TIMEOUT_MS = 10_000;

    private static final int HELLO = 0x01;
    private static final int SET_ROTATION = 0x10;
    private static final int ADAPT_DISPLAY = 0x11;
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
    private static final int ESTATE = 0x04;
    private static final int EFAIL = 0xFF;

    private static final int CAP_VIDEO = 1 << 0;
    private static final int CAP_INPUT = 1 << 2;
    private static final int CAPABILITIES = CAP_VIDEO | CAP_INPUT;
    private static final int ROTATION_RELEASE = 0xFF;

    private static volatile boolean helloDone;
    private static volatile int videoMaxMessage;

    private final OutputStream out;
    private boolean hello;
    private long replyMax = 0xFFFFFFFFL;
    private Input input;
    private boolean inputFailed;
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
            refuse(new LocalServerSocket(name + "-audio"));
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
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            RotationLock.release();
            DisplaySize.release();
        }));
        System.out.println("listening");
        System.out.flush();
        Server server = null;
        try (LocalSocket socket = control.accept()) {
            watchdog.interrupt();
            control.close();
            server = new Server(socket.getOutputStream());
            server.serve(new DataInputStream(socket.getInputStream()));
        } catch (EOFException e) {
            System.out.println("closed");
        } catch (IOException e) {
            System.out.println("control: " + e);
        }
        if (server != null && server.input != null) {
            server.input.clear();
        }
        System.exit(0);
    }

    static boolean helloDone() {
        return helloDone;
    }

    static int videoMaxMessage() {
        return videoMaxMessage;
    }

    // No audio is served: every connection is closed.
    private static void refuse(LocalServerSocket server) {
        Thread thread = new Thread(() -> {
            while (true) {
                try {
                    server.accept().close();
                } catch (IOException e) {
                    return;
                }
            }
        });
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
            if (!hello && kind != HELLO) {
                throw new IOException("type " + kind + " before HELLO");
            }
            if (kind == HELLO) {
                onHello(id, payload);
            } else if (kind == SET_ROTATION) {
                onSetRotation(id, payload);
            } else if (kind == ADAPT_DISPLAY) {
                onAdaptDisplay(id, payload);
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
        result.putInt(CAPABILITIES);
        result.putInt(MAX_MESSAGE);
        if (payload.length >= 12) {
            replyMax = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN).getInt(8) & 0xFFFFFFFFL;
        }
        videoMaxMessage = payload.length >= 16 ? ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN).getInt(12) : 0;
        reply(id, OK, result.array());
        hello = true;
        helloDone = true;
        System.out.println("HELLO ok");
        System.out.flush();
    }

    private void onSetRotation(int id, byte[] payload) throws IOException {
        int rotation = payload.length >= 1 ? payload[0] & 0xFF : -1;
        if (rotation == ROTATION_RELEASE) {
            RotationLock.release();
            reply(id, OK, new byte[0]);
            return;
        }
        if (rotation < 0 || rotation > 3) {
            reply(id, EINVAL, new byte[0]);
            return;
        }
        try {
            RotationLock.hold(rotation);
            reply(id, OK, new byte[0]);
        } catch (Throwable t) {
            System.out.println("rotation: " + t);
            reply(id, EFAIL, new byte[0]);
        }
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
            reply(id, OK, new byte[0]);
            return;
        }
        if (width == 0 || height == 0) {
            reply(id, EINVAL, new byte[0]);
            return;
        }
        try {
            reply(id, DisplaySize.adapt(width, height) ? OK : ESTATE, new byte[0]);
        } catch (Throwable t) {
            System.out.println("adapt display: " + t);
            reply(id, EFAIL, new byte[0]);
        }
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
        ByteBuffer m = ByteBuffer.allocate(HEADER_BYTES + 4 + result.length).order(ByteOrder.LITTLE_ENDIAN);
        m.putInt(4 + result.length).put((byte) REPLY).put((byte) 0).putShort((short) id);
        m.put((byte) status).put((byte) 0).putShort((short) 0);
        m.put(result);
        synchronized (out) {
            out.write(m.array());
            out.flush();
        }
    }
}
