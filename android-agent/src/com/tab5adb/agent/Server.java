package com.tab5adb.agent;

import android.graphics.Point;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.Build;

import java.io.DataInputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * tab5adb-agent — Android-side companion for Tab5-ADB.
 *
 * Launched via app_process (scrcpy style, no APK), it runs with shell uid and
 * listens on the abstract socket {@code localabstract:tab5adb-agent}, which the
 * Tab5 host reaches over its embedded ADB.
 *
 * <p>This milestone implements the protocol's HELLO handshake (android-agent/
 * docs/protocol.md §3/§4): on each connection the agent sends its HELLO
 * CONTROL_REQUEST and reads the Tab5 CONTROL_RESPONSE, checking the proto
 * version matches. The JPEG strip stream (Phase 2) comes after. Source size is
 * best-effort (informational in HELLO); a failure falls back to 0x0.
 */
public final class Server {
    private static final String SOCKET_NAME = "tab5adb-agent";

    // Wire constants — keep in sync with android-agent/docs/protocol.md and
    // components/agent_link/inc/agent_link_protocol.hpp.
    private static final int MAGIC = 0xA5;
    private static final int TYPE_CONTROL_REQUEST = 0x01;
    private static final int TYPE_CONTROL_RESPONSE = 0x02;
    private static final int CMD_HELLO = 0x01;
    private static final int STATUS_OK = 0x00;
    private static final int PROTO_VERSION = 1;
    private static final int VIDEO_CODEC_JPEG = 0x01;

    private static final int AGENT_VER_MAJOR = 0;
    private static final int AGENT_VER_MINOR = 1;
    private static final int AGENT_VER_PATCH = 0;

    public static void main(String[] args) throws Exception {
        System.out.println("tab5adb-agent: listening on localabstract:" + SOCKET_NAME);

        LocalServerSocket server = new LocalServerSocket(SOCKET_NAME);
        while (true) {
            LocalSocket client = server.accept();
            System.out.println("tab5adb-agent: client connected");
            try {
                serve(client);
            } catch (Exception e) {
                System.err.println("tab5adb-agent: " + e);
            } finally {
                client.close();
                System.out.println("tab5adb-agent: client disconnected");
            }
        }
    }

    /** Run the HELLO handshake, then hold the connection until the peer closes. */
    private static void serve(LocalSocket client) throws Exception {
        OutputStream out = client.getOutputStream();
        DataInputStream in = new DataInputStream(client.getInputStream());

        sendHelloRequest(out);

        // Read the Tab5 HELLO response and validate it (protocol.md §4.4).
        Frame resp = readFrame(in);
        if (resp.type != TYPE_CONTROL_RESPONSE) {
            throw new IllegalStateException("expected CONTROL_RESPONSE, got type " + resp.type);
        }
        byte[] p = resp.payload;
        if (p.length < 3 || (p[0] & 0xFF) != CMD_HELLO) {
            throw new IllegalStateException("malformed HELLO response");
        }
        int status = p[2] & 0xFF;
        if (status != STATUS_OK || p.length < 3 + 12) {
            throw new IllegalStateException("HELLO rejected (status " + status + ")");
        }
        long maxPayload = readU32(p, 3);
        int targetW = readU16(p, 7);
        int targetH = readU16(p, 9);
        int proto = p[11] & 0xFF;
        int scaleMode = p[12] & 0xFF;
        if (proto != PROTO_VERSION) {
            throw new IllegalStateException("proto mismatch: agent " + PROTO_VERSION + " vs Tab5 " + proto);
        }

        System.out.println("tab5adb-agent: HELLO ok"
                + " max_payload=" + maxPayload
                + " target=" + targetW + "x" + targetH
                + " scale_mode=" + scaleMode);

        // Milestone: handshake done. Hold the stream open (the JPEG stream would
        // start here in Phase 2); let the Tab5 side drive teardown.
        byte[] scratch = new byte[256];
        InputStream raw = client.getInputStream();
        while (raw.read(scratch) >= 0) {
            // ignore further bytes (future: Tab5 -> agent control)
        }
    }

    private static void sendHelloRequest(OutputStream out) throws Exception {
        int[] size = displaySize();
        byte[] args = new byte[10];
        args[0] = (byte) PROTO_VERSION;
        args[1] = (byte) AGENT_VER_MAJOR;
        args[2] = (byte) AGENT_VER_MINOR;
        args[3] = (byte) AGENT_VER_PATCH;
        writeU16(args, 4, size[0]);
        writeU16(args, 6, size[1]);
        args[8] = (byte) VIDEO_CODEC_JPEG;
        args[9] = 0;  // reserved

        byte[] payload = new byte[2 + args.length];
        payload[0] = (byte) CMD_HELLO;
        payload[1] = (byte) 0x01;  // req_id
        System.arraycopy(args, 0, payload, 2, args.length);

        writeFrame(out, TYPE_CONTROL_REQUEST, 0, 0, payload);
        System.out.println("tab5adb-agent: sent HELLO (source=" + size[0] + "x" + size[1] + ")");
    }

    // --- framing (protocol.md §3) ---

    private static final class Frame {
        int type;
        int flags;
        int seq;
        byte[] payload;
    }

    private static void writeFrame(OutputStream out, int type, int flags, int seq,
                                   byte[] payload) throws Exception {
        byte[] hdr = new byte[8];
        hdr[0] = (byte) MAGIC;
        hdr[1] = (byte) type;
        hdr[2] = (byte) flags;
        hdr[3] = (byte) seq;
        writeU32(hdr, 4, payload.length);
        out.write(hdr);
        out.write(payload);
        out.flush();
    }

    private static Frame readFrame(DataInputStream in) throws Exception {
        byte[] hdr = new byte[8];
        in.readFully(hdr);
        if ((hdr[0] & 0xFF) != MAGIC) {
            throw new IllegalStateException("bad MAGIC 0x" + Integer.toHexString(hdr[0] & 0xFF));
        }
        Frame f = new Frame();
        f.type = hdr[1] & 0xFF;
        f.flags = hdr[2] & 0xFF;
        f.seq = hdr[3] & 0xFF;
        long len = readU32(hdr, 4);
        f.payload = new byte[(int) len];
        in.readFully(f.payload);
        return f;
    }

    // --- little-endian helpers ---

    private static void writeU16(byte[] b, int off, int v) {
        b[off] = (byte) v;
        b[off + 1] = (byte) (v >> 8);
    }

    private static void writeU32(byte[] b, int off, long v) {
        b[off] = (byte) v;
        b[off + 1] = (byte) (v >> 8);
        b[off + 2] = (byte) (v >> 16);
        b[off + 3] = (byte) (v >> 24);
    }

    private static int readU16(byte[] b, int off) {
        return (b[off] & 0xFF) | ((b[off + 1] & 0xFF) << 8);
    }

    private static long readU32(byte[] b, int off) {
        return (b[off] & 0xFFL) | ((b[off + 1] & 0xFFL) << 8)
                | ((b[off + 2] & 0xFFL) << 16) | ((b[off + 3] & 0xFFL) << 24);
    }

    /**
     * Best-effort physical display size via hidden-API reflection (app_process
     * shell uid reaches these). Informational in HELLO, so any failure falls back
     * to 0x0 rather than aborting the handshake.
     */
    private static int[] displaySize() {
        try {
            Class<?> dmg = Class.forName("android.hardware.display.DisplayManagerGlobal");
            Object inst = dmg.getMethod("getInstance").invoke(null);
            Object display = dmg.getMethod("getRealDisplay", int.class).invoke(inst, 0);
            Point pt = new Point();
            display.getClass().getMethod("getRealSize", Point.class).invoke(display, pt);
            return new int[]{pt.x, pt.y};
        } catch (Throwable t) {
            System.err.println("tab5adb-agent: displaySize failed: " + t);
            return new int[]{0, 0};
        }
    }
}
