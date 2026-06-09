package com.tab5adb.agent;

import android.graphics.Bitmap;
import android.graphics.Point;
import android.net.LocalServerSocket;
import android.net.LocalSocket;

import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.util.List;

/**
 * tab5adb-agent — Android-side companion for Tab5-ADB.
 *
 * Launched via app_process (scrcpy style, no APK), it runs with shell uid and
 * listens on the abstract socket {@code localabstract:tab5adb-agent}, which the
 * Tab5 host reaches over its embedded ADB.
 *
 * <p>Protocol (android-agent/docs/protocol.md): on each connection the agent
 * sends its HELLO CONTROL_REQUEST (link establishment only — proto / version /
 * capability, §4.4), reads the Tab5 HELLO response, then waits for the Tab5 to
 * send MIRROR_START (panel size / scale mode / streams). It answers MIRROR_START
 * and streams the screen as JPEG strips (§5) until the peer closes.
 *
 * <p>{@code --test-pattern} streams a deterministic {@link TestPattern} instead of
 * the real screen, so the headless test can verify the pipeline + framing without
 * the hidden capture APIs. {@code --test-size WxH} sets the test source size.
 */
public final class Server {
    private static final String SOCKET_NAME = "tab5adb-agent";

    // Wire constants — keep in sync with android-agent/docs/protocol.md and
    // components/agent_link/inc/agent_link_protocol.hpp.
    private static final int MAGIC = 0xA5;
    private static final int TYPE_CONTROL_REQUEST = 0x01;
    private static final int TYPE_CONTROL_RESPONSE = 0x02;
    private static final int TYPE_JPEG = 0x10;
    private static final int FLAG_FRAME_START = 0x01;
    private static final int FLAG_FRAME_END = 0x02;
    private static final int CMD_HELLO = 0x01;
    private static final int CMD_MIRROR_START = 0x10;
    private static final int STATUS_OK = 0x00;
    private static final int STATUS_ENOTSUP = 0x02;
    private static final int PROTO_VERSION = 1;
    private static final int CAP_VIDEO = 0x0001;
    private static final int VIDEO_CODEC_JPEG = 0x01;

    private static final int AGENT_VER_MAJOR = 0;
    private static final int AGENT_VER_MINOR = 2;
    private static final int AGENT_VER_PATCH = 0;

    // Mirror stream defaults (§5.1).
    private static final int SPLIT_COUNT = 4;
    private static final int JPEG_QUALITY = 60;
    // 0 = no artificial pacing: stream as fast as the capture + pipeline produce
    // frames (the encoder/capture rate is the cap). Set >0 to throttle.
    private static final int TARGET_FPS = 0;

    private final boolean testPattern;
    private final int testW;
    private final int testH;

    private Server(boolean testPattern, int testW, int testH) {
        this.testPattern = testPattern;
        this.testW = testW;
        this.testH = testH;
    }

    public static void main(String[] args) throws Exception {
        boolean testPattern = false;
        int testW = 1080, testH = 2160;  // portrait; differs from 9:16 to exercise fit
        for (int i = 0; i < args.length; i++) {
            if ("--test-pattern".equals(args[i])) {
                testPattern = true;
            } else if ("--test-size".equals(args[i]) && i + 1 < args.length) {
                String[] wh = args[++i].split("x");
                testW = Integer.parseInt(wh[0]);
                testH = Integer.parseInt(wh[1]);
            }
        }
        System.out.println("tab5adb-agent: listening on localabstract:" + SOCKET_NAME
                + (testPattern ? " [test-pattern " + testW + "x" + testH + "]" : ""));

        Server server = new Server(testPattern, testW, testH);
        LocalServerSocket sock = new LocalServerSocket(SOCKET_NAME);
        while (true) {
            LocalSocket client = sock.accept();
            System.out.println("tab5adb-agent: client connected");
            try {
                server.serve(client);
            } catch (Exception e) {
                System.err.println("tab5adb-agent: " + e);
            } finally {
                client.close();
                System.out.println("tab5adb-agent: client disconnected");
            }
        }
    }

    /** HELLO handshake, MIRROR_START handshake, then stream the screen as JPEG. */
    private void serve(LocalSocket client) throws Exception {
        Conn conn = new Conn(client.getOutputStream(),
                new DataInputStream(client.getInputStream()));

        helloHandshake(conn);
        MirrorParams mp = mirrorHandshake(conn);
        if (mp == null) return;  // MIRROR_START rejected / no video requested
        streamVideo(conn, mp);
    }

    // --- HELLO (link establishment, §4.4) ---

    private void helloHandshake(Conn conn) throws Exception {
        byte[] args = new byte[8];
        args[0] = (byte) PROTO_VERSION;
        args[1] = (byte) AGENT_VER_MAJOR;
        args[2] = (byte) AGENT_VER_MINOR;
        args[3] = (byte) AGENT_VER_PATCH;
        writeU16(args, 4, CAP_VIDEO);  // capabilities
        writeU16(args, 6, 0);          // reserved
        conn.sendControlRequest(CMD_HELLO, 0x01, args);
        System.out.println("tab5adb-agent: sent HELLO (caps=video)");

        Frame resp = conn.readFrame();
        if (resp.type != TYPE_CONTROL_RESPONSE) {
            throw new IllegalStateException("expected CONTROL_RESPONSE, got type " + resp.type);
        }
        byte[] p = resp.payload;
        if (p.length < 3 || (p[0] & 0xFF) != CMD_HELLO) {
            throw new IllegalStateException("malformed HELLO response");
        }
        int status = p[2] & 0xFF;
        if (status != STATUS_OK || p.length < 3 + 8) {
            throw new IllegalStateException("HELLO rejected (status " + status + ")");
        }
        int proto = p[3] & 0xFF;
        int caps = readU16(p, 5);
        long maxPayload = readU32(p, 7);
        if (proto != PROTO_VERSION) {
            throw new IllegalStateException("proto mismatch: agent " + PROTO_VERSION + " vs Tab5 " + proto);
        }
        conn.maxPayload = maxPayload;
        System.out.println("tab5adb-agent: HELLO ok caps=0x" + Integer.toHexString(caps)
                + " max_payload=" + maxPayload);
    }

    // --- MIRROR_START (start mirror, §4.4) ---

    private static final class MirrorParams {
        int targetW, targetH, scaleMode, streams;
    }

    /** Wait for the Tab5 MIRROR_START, answer it, return the params (null if no video). */
    private MirrorParams mirrorHandshake(Conn conn) throws Exception {
        Frame req = conn.readFrame();
        if (req.type != TYPE_CONTROL_REQUEST) {
            throw new IllegalStateException("expected CONTROL_REQUEST, got type " + req.type);
        }
        byte[] p = req.payload;
        if (p.length < 2 || (p[0] & 0xFF) != CMD_MIRROR_START) {
            throw new IllegalStateException("expected MIRROR_START, got cmd "
                    + (p.length > 0 ? (p[0] & 0xFF) : -1));
        }
        int reqId = p[1] & 0xFF;
        if (p.length < 2 + 8) {
            throw new IllegalStateException("malformed MIRROR_START");
        }
        MirrorParams mp = new MirrorParams();
        mp.targetW = readU16(p, 2);
        mp.targetH = readU16(p, 4);
        mp.scaleMode = p[6] & 0xFF;
        mp.streams = p[7] & 0xFF;
        System.out.println("tab5adb-agent: MIRROR_START target=" + mp.targetW + "x" + mp.targetH
                + " scale=" + mp.scaleMode + " streams=0x" + Integer.toHexString(mp.streams));

        if ((mp.streams & CAP_VIDEO) == 0) {  // we only offer video today
            conn.sendControlResponse(CMD_MIRROR_START, reqId, STATUS_ENOTSUP, null);
            return null;
        }

        int[] src = sourceSize();
        byte[] result = new byte[8];
        writeU16(result, 0, src[0]);          // source_width
        writeU16(result, 2, src[1]);          // source_height
        result[4] = (byte) VIDEO_CODEC_JPEG;  // video_codec
        result[5] = 0;                        // reserved
        writeU16(result, 6, 0);               // reserved
        conn.sendControlResponse(CMD_MIRROR_START, reqId, STATUS_OK, result);
        return mp;
    }

    // --- video stream (§5) ---

    private void streamVideo(Conn conn, MirrorParams mp) throws Exception {
        FramePipeline pipeline = new FramePipeline(
                mp.targetW, mp.targetH, mp.scaleMode, SPLIT_COUNT, JPEG_QUALITY);
        ScreenCapture capture = null;
        if (!testPattern) {
            int[] src = sourceSize();
            // The GPU does rotate/scale-fit/letterbox into a panel-sized frame.
            capture = new ScreenCapture(src[0], src[1], mp.targetW, mp.targetH, mp.scaleMode);
        }
        System.out.println("tab5adb-agent: streaming video (split=" + SPLIT_COUNT
                + " q=" + JPEG_QUALITY + (testPattern ? ", test-pattern)" : ", screen)"));

        long frameNs = TARGET_FPS > 0 ? 1_000_000_000L / TARGET_FPS : 0;
        int frame = 0;
        try {
            while (!Thread.interrupted()) {
                long t0 = System.nanoTime();
                Bitmap src = nextSource(capture, frame);
                if (src == null) {  // capture not ready yet — wait for the first frame
                    sleep(10);
                    continue;
                }
                List<FramePipeline.Strip> strips;
                try {
                    // Real capture is already a panel-sized frame (GPU geometry) → just
                    // strip it; the test pattern is a raw source → run the CPU pipeline.
                    strips = testPattern ? pipeline.process(src) : pipeline.stripsOf(src);
                } finally {
                    src.recycle();
                }
                sendFrame(conn, strips);
                frame++;

                if (frameNs > 0) {
                    long dt = System.nanoTime() - t0;
                    if (dt < frameNs) sleep((frameNs - dt) / 1_000_000L);
                }
            }
        } catch (IOException eof) {
            // peer closed the stream — normal teardown
        } finally {
            if (capture != null) capture.close();
        }
        System.out.println("tab5adb-agent: stream ended after " + frame + " frames");
    }

    private Bitmap nextSource(ScreenCapture capture, int frame) {
        if (testPattern) return TestPattern.make(testW, testH, frame);
        return capture.acquire();
    }

    /** Send one screen frame as a run of JPEG strips (FRAME_START..FRAME_END, §5.2). */
    private void sendFrame(Conn conn, List<FramePipeline.Strip> strips) throws IOException {
        int n = strips.size();
        for (int i = 0; i < n; i++) {
            FramePipeline.Strip s = strips.get(i);
            byte[] payload = new byte[8 + s.jpeg.length];
            writeU16(payload, 0, s.x);
            writeU16(payload, 2, s.y);
            writeU16(payload, 4, s.w);
            writeU16(payload, 6, s.h);
            System.arraycopy(s.jpeg, 0, payload, 8, s.jpeg.length);
            if (conn.maxPayload > 0 && payload.length > conn.maxPayload) {
                // Tab5 would reject an over-cap frame (§3); skip the whole frame and
                // log rather than corrupt the stream. SPLIT_COUNT should keep us under.
                System.err.println("tab5adb-agent: strip " + payload.length
                        + "B exceeds max_payload " + conn.maxPayload + " — dropping frame");
                return;
            }
            int flags = (i == 0 ? FLAG_FRAME_START : 0) | (i == n - 1 ? FLAG_FRAME_END : 0);
            conn.writeFrame(TYPE_JPEG, flags, payload);
        }
    }

    // --- per-connection framing (§3) ---

    private static final class Conn {
        final OutputStream out;
        final DataInputStream in;
        int outSeq = 0;        // outgoing frame counter (§3 SEQ), one per direction
        long maxPayload = 0;   // Tab5's accepted payload cap (from HELLO)

        Conn(OutputStream out, DataInputStream in) {
            this.out = out;
            this.in = in;
        }

        void sendControlRequest(int cmd, int reqId, byte[] args) throws IOException {
            byte[] payload = new byte[2 + (args == null ? 0 : args.length)];
            payload[0] = (byte) cmd;
            payload[1] = (byte) reqId;
            if (args != null) System.arraycopy(args, 0, payload, 2, args.length);
            writeFrame(TYPE_CONTROL_REQUEST, 0, payload);
        }

        void sendControlResponse(int cmd, int reqId, int status, byte[] result) throws IOException {
            byte[] payload = new byte[3 + (result == null ? 0 : result.length)];
            payload[0] = (byte) cmd;
            payload[1] = (byte) reqId;
            payload[2] = (byte) status;
            if (result != null) System.arraycopy(result, 0, payload, 3, result.length);
            writeFrame(TYPE_CONTROL_RESPONSE, 0, payload);
        }

        void writeFrame(int type, int flags, byte[] payload) throws IOException {
            byte[] hdr = new byte[8];
            hdr[0] = (byte) MAGIC;
            hdr[1] = (byte) type;
            hdr[2] = (byte) flags;
            hdr[3] = (byte) (outSeq++ & 0xFF);
            writeU32(hdr, 4, payload.length);
            out.write(hdr);
            out.write(payload);
            out.flush();
        }

        Frame readFrame() throws IOException {
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
    }

    private static final class Frame {
        int type;
        int flags;
        int seq;
        byte[] payload;
    }

    // --- helpers ---

    private static void sleep(long ms) {
        if (ms <= 0) return;
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

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
     * Physical display size via hidden-API reflection (app_process shell uid
     * reaches these). For the test pattern, use the configured test source size.
     * A capture failure later still needs a real size, so this throws on failure
     * in the real path; the test path never calls into capture.
     */
    private int[] sourceSize() {
        if (testPattern) return new int[]{testW, testH};
        try {
            Class<?> dmg = Class.forName("android.hardware.display.DisplayManagerGlobal");
            Object inst = dmg.getMethod("getInstance").invoke(null);
            Object display = dmg.getMethod("getRealDisplay", int.class).invoke(inst, 0);
            Point pt = new Point();
            display.getClass().getMethod("getRealSize", Point.class).invoke(display, pt);
            if (pt.x <= 0 || pt.y <= 0) throw new IllegalStateException("bad size " + pt.x + "x" + pt.y);
            return new int[]{pt.x, pt.y};
        } catch (Throwable t) {
            throw new RuntimeException("displaySize failed: " + t, t);
        }
    }
}
