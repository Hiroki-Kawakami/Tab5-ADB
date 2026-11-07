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
    private static final int VERSION_MINOR = 1;
    private static final int VERSION_PATCH = 0;

    private static final int PROTO_VERSION = 1;
    private static final int HEADER_BYTES = 8;
    private static final int MAX_MESSAGE = 64 * 1024;
    private static final long CONNECT_TIMEOUT_MS = 10_000;

    private static final int HELLO = 0x01;
    private static final int REQUEST_LAST = 0x3F;
    private static final int REPLY = 0x40;

    private static final int OK = 0x00;
    private static final int ENOTSUP = 0x02;

    private static final int CAPABILITIES = 0;

    private final OutputStream out;
    private boolean hello;

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
            refuse(new LocalServerSocket(name + "-video"));
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

    // No video or audio is served: every connection is closed.
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
        reply(id, OK, result.array());
        hello = true;
        System.out.println("HELLO ok");
        System.out.flush();
    }

    private void reply(int id, int status, byte[] result) throws IOException {
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
