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
 * and streams the screen as JPEG strips (§5) until it receives MIRROR_STOP — at
 * which point it stops streaming and returns to READY (the socket stays open, so
 * a later MIRROR_START resumes), or until the peer closes the socket.
 *
 * <p>Control and video run concurrently (§4.4): a dedicated reader thread reads
 * every inbound frame (the HELLO response, MIRROR_START, MIRROR_STOP) while the
 * main thread sends the JPEG stream, so MIRROR_STOP is never blocked behind the
 * video flow. Frame writes from both threads are serialized in {@link Conn}.
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
    private static final int TYPE_EVENT = 0x03;
    private static final int TYPE_INPUT = 0x04;
    private static final int TYPE_JPEG = 0x10;
    private static final int TYPE_AUDIO = 0x11;
    private static final int INPUT_KEY = 0x00;  // input_type (§4.7)
    private static final int INPUT_TOUCH = 0x01;  // input_type (§4.7)
    private static final int INPUT_TOUCH_BATCH = 0x03;  // input_type (§4.7)
    private static final int TOUCH_BATCH_RECORD_LEN = 6;  // action+ptr+x+y
    private static final int EVENT_ORIENTATION = 0x03;
    private static final int FLAG_FRAME_START = 0x01;
    private static final int FLAG_FRAME_END = 0x02;
    private static final int CMD_HELLO = 0x01;
    private static final int CMD_MIRROR_START = 0x10;
    private static final int CMD_MIRROR_STOP = 0x11;
    private static final int CMD_GET_APP_LIST = 0x20;
    private static final int CMD_GET_APP_ICON = 0x21;
    private static final int STATUS_OK = 0x00;
    private static final int STATUS_EINVAL = 0x01;
    private static final int STATUS_ENOTSUP = 0x02;
    private static final int STATUS_EFAIL = 0xFF;
    private static final int PROTO_VERSION = 1;
    private static final int CAP_VIDEO = 0x0001;
    private static final int CAP_AUDIO = 0x0002;
    private static final int CAP_APPINFO = 0x0004;
    private static final int VIDEO_CODEC_JPEG = 0x01;
    private static final int SCALE_ASPECT = 2;  // scale_mode (§5.3)
    private static final int SCALE_ADAPT = 3;   // scale_mode (§5.3): resize the source

    private static final int AGENT_VER_MAJOR = 0;
    private static final int AGENT_VER_MINOR = 5;
    private static final int AGENT_VER_PATCH = 0;

    // Mirror stream defaults (§5.1).
    private static final int SPLIT_COUNT = 4;
    private static final int JPEG_QUALITY = 80;
    // Upper FPS bound: the Tab5 panel tops out at 60fps, so cap the stream there so
    // a high-refresh source (e.g. a 120Hz phone) can't push frames the panel can't
    // show. The pacing only applies to frames actually sent (a static screen yields
    // no new capture frame, so nothing is paced/sent). 0 = no cap (encoder/capture
    // rate is the only limit).
    private static final int TARGET_FPS = 60;

    private final boolean testPattern;
    private final boolean testTone;  // stream a deterministic sine instead of REMOTE_SUBMIX
    private final int testW;
    private final int testH;

    // Input injector (§4.7), built once on the main thread (initInput) before any
    // reader thread starts, then read by the reader thread. Null = injection
    // unavailable (setup failed), so INPUT frames are silently dropped.
    private Input input;

    // App metadata service (§4.4 GET_APP_LIST / GET_APP_ICON), built once on the
    // main thread like `input` (SystemContext needs the main Looper). Null = the
    // PackageManager is unreachable; the HELLO then drops the APPINFO capability
    // and the commands answer ENOTSUP.
    private AppInfo appInfo;

    private Server(boolean testPattern, boolean testTone, int testW, int testH) {
        this.testPattern = testPattern;
        this.testTone = testTone;
        this.testW = testW;
        this.testH = testH;
    }

    public static void main(String[] args) throws Exception {
        boolean testPattern = false;
        boolean testTone = false;
        int testW = 1080, testH = 2160;  // portrait; differs from 9:16 to exercise fit
        for (int i = 0; i < args.length; i++) {
            if ("--test-pattern".equals(args[i])) {
                testPattern = true;
            } else if ("--test-tone".equals(args[i])) {
                testTone = true;
            } else if ("--test-size".equals(args[i]) && i + 1 < args.length) {
                String[] wh = args[++i].split("x");
                testW = Integer.parseInt(wh[0]);
                testH = Integer.parseInt(wh[1]);
            }
        }
        // The framework's InputManager / system Context creation (§4.7 input
        // injection) needs a Looper on the constructing thread: `new ActivityThread()`
        // builds an internal Handler. Establish a main Looper once at startup (scrcpy
        // does the same); we never run its loop — injectInputEvent is ASYNC, so no
        // message pump is needed. Harmless to the capture path (it polls acquire()).
        if (android.os.Looper.getMainLooper() == null) {
            android.os.Looper.prepareMainLooper();
        }

        System.out.println("tab5adb-agent: listening on localabstract:" + SOCKET_NAME
                + (testPattern ? " [test-pattern " + testW + "x" + testH + "]" : ""));

        Server server = new Server(testPattern, testTone, testW, testH);
        // Build the input injector here on the main thread (it constructs an
        // ActivityThread whose internal Handler needs *this* thread's Looper). The
        // ActivityThread is a process singleton, so the reader thread then just
        // reuses the injector for a pure binder injectInputEvent (no Looper needed).
        server.initInput();
        server.initAppInfo();
        // Restore any Adapt-mode display override on JVM exit (SIGTERM/SIGHUP, e.g. the
        // launch shell closing). serve() already restores on a clean disconnect; this
        // is the backstop for an abrupt teardown that skips serve()'s finally.
        final Server srv = server;
        Runtime.getRuntime().addShutdownHook(new Thread(srv::clearAdaptSize));
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

    /**
     * One connection: send HELLO, then a control reader thread reads every inbound
     * frame while this (main) thread runs the session loop — wait for MIRROR_START,
     * stream JPEG until MIRROR_STOP (back to READY) or the peer closes, repeat.
     */
    private void serve(LocalSocket client) throws Exception {
        Conn conn = new Conn(client.getOutputStream(),
                new DataInputStream(client.getInputStream()));

        // Agent-initiated HELLO request (§4.4); its response is read by the reader.
        int caps = CAP_VIDEO | (audioAvailable() ? CAP_AUDIO : 0)
                | (appInfo != null ? CAP_APPINFO : 0);
        byte[] args = new byte[8];
        args[0] = (byte) PROTO_VERSION;
        args[1] = (byte) AGENT_VER_MAJOR;
        args[2] = (byte) AGENT_VER_MINOR;
        args[3] = (byte) AGENT_VER_PATCH;
        writeU16(args, 4, caps);  // capabilities
        writeU16(args, 6, 0);     // reserved
        conn.sendControlRequest(CMD_HELLO, 0x01, args);
        System.out.println("tab5adb-agent: sent HELLO (caps=0x" + Integer.toHexString(caps) + ")");

        Thread reader = new Thread(() -> readLoop(conn), "tab5adb-control-reader");
        reader.setDaemon(true);
        reader.start();

        // Wait for the HELLO response (or a disconnect) before serving features.
        synchronized (conn.lock) {
            while (!conn.helloOk && !conn.closed) conn.lock.wait();
        }

        // Session loop: each MIRROR_START streams until MIRROR_STOP / disconnect,
        // then we return to READY and wait for the next MIRROR_START on this link.
        try {  // restore any adapt override (clearAdaptSize) on session exit, below
        while (!conn.closed) {
            MirrorParams mp;
            synchronized (conn.lock) {
                while (conn.pendingStart == null && !conn.closed) conn.lock.wait();
                mp = conn.pendingStart;
                conn.pendingStart = null;
            }
            if (conn.closed) break;
            // Audio (§6) runs on its own thread alongside the video flow, started
            // for this MIRROR_START and torn down when streaming ends (MIRROR_STOP,
            // reconfigure, or disconnect). A real-capture init failure degrades to
            // video-only (the Tab5's audio player just stays silent).
            AudioStreamer audio = null;
            if (mp.audio) {
                try {
                    audio = new AudioStreamer(conn);
                    audio.start();
                } catch (Throwable t) {
                    System.err.println("tab5adb-agent: audio start failed, video-only: " + t);
                    audio = null;
                }
            }
            try {
                streamVideo(conn, mp);  // returns on MIRROR_STOP (READY) / reconfigure / disconnect
            } finally {
                if (audio != null) audio.stop();
            }
            // streamVideo returned: MIRROR_STOP (READY) / reconfigure / disconnect. A
            // reconfigure's MIRROR_START already set the adapt state for the next mp;
            // STOP or disconnect (no pendingStart) restores the device size now.
            if (conn.pendingStart == null) clearAdaptSize();
        }
        } finally {
            clearAdaptSize();  // backstop: restore the device size on any session exit
        }
        reader.join();
    }

    // --- control reader thread: reads every inbound frame, dispatches by TYPE ---

    /** Read frames until the peer closes; on exit mark the connection closed. */
    private void readLoop(Conn conn) {
        try {
            while (true) handleFrame(conn, conn.readFrame());
        } catch (IOException eof) {
            // peer closed the stream — normal teardown
        } catch (Exception e) {
            System.err.println("tab5adb-agent: reader: " + e);
        } finally {
            synchronized (conn.lock) {
                conn.closed = true;
                conn.lock.notifyAll();
            }
        }
    }

    private void handleFrame(Conn conn, Frame f) throws IOException {
        if (f.type == TYPE_CONTROL_RESPONSE) {
            handleHelloResponse(conn, f);
        } else if (f.type == TYPE_CONTROL_REQUEST) {
            byte[] p = f.payload;
            if (p.length < 2) throw new IllegalStateException("short control request");
            int cmd = p[0] & 0xFF, reqId = p[1] & 0xFF;
            if (cmd == CMD_MIRROR_START) handleMirrorStart(conn, p, reqId);
            else if (cmd == CMD_MIRROR_STOP) handleMirrorStop(conn, reqId);
            else if (cmd == CMD_GET_APP_LIST) handleGetAppList(conn, reqId);
            else if (cmd == CMD_GET_APP_ICON) handleGetAppIcon(conn, p, reqId);
            // unknown cmd: ignore (forward compat, §4.4)
        } else if (f.type == TYPE_INPUT) {
            handleInput(conn, f.payload);
        }
        // unknown TYPE / EVENT: ignore (§3.1)
    }

    // --- input injection (§4.7) — fire-and-forget, no response ---

    /** Parse a TYPE=INPUT frame and inject it on the source device. */
    private void handleInput(Conn conn, byte[] p) {
        if (p.length < 1) return;
        int inputType = p[0] & 0xFF;
        if (inputType == INPUT_KEY) {
            if (p.length < 1 + 13) {  // input_type + INPUT_KEY args (§4.7)
                System.err.println("tab5adb-agent: short INPUT_KEY");
                return;
            }
            int action = p[1] & 0xFF;
            int keycode = (int) readU32(p, 2);
            int repeat = (int) readU32(p, 6);
            int meta = (int) readU32(p, 10);
            if (input != null) input.injectKey(action, keycode, repeat, meta);
        } else if (inputType == INPUT_TOUCH) {
            if (p.length < 1 + 7) {  // input_type + INPUT_TOUCH args (§4.7)
                System.err.println("tab5adb-agent: short INPUT_TOUCH");
                return;
            }
            int action = p[1] & 0xFF;   // 0=DOWN, 1=MOVE, 2=UP
            int pointerId = p[2] & 0xFF;
            // p[3] reserved
            int px = readU16(p, 4);     // Tab5 panel coords
            int py = readU16(p, 6);
            handleTouch(conn, action, pointerId, px, py);
        } else if (inputType == INPUT_TOUCH_BATCH) {
            // count, then `count` packed records — replay each through handleTouch in
            // order (same per-pointer state machine as a run of INPUT_TOUCH frames).
            if (p.length < 2) {
                System.err.println("tab5adb-agent: short INPUT_TOUCH_BATCH");
                return;
            }
            int count = p[1] & 0xFF;
            if (p.length < 2 + count * TOUCH_BATCH_RECORD_LEN) {
                System.err.println("tab5adb-agent: truncated INPUT_TOUCH_BATCH");
                return;
            }
            int off = 2;
            for (int i = 0; i < count; i++) {
                int action = p[off] & 0xFF;
                int pointerId = p[off + 1] & 0xFF;
                int px = readU16(p, off + 2);
                int py = readU16(p, off + 4);
                handleTouch(conn, action, pointerId, px, py);
                off += TOUCH_BATCH_RECORD_LEN;
            }
        }
        // unknown input_type: ignore (forward compat, §4.7)
    }

    /**
     * Map a Tab5 panel-coord touch to the source's logical display and inject it
     * (§4.7). The Tab5 owns the gesture (per-pointer DOWN/MOVE/UP); the agent owns
     * the geometry, so it inverts panel -> source via {@link Projection}. UP always
     * goes through (so a pointer is released even if it lifted in the letterbox or
     * after the stream stopped); DOWN/MOVE need live geometry and a point inside
     * the image.
     */
    private void handleTouch(Conn conn, int action, int pointerId, int px, int py) {
        if (input == null) return;
        if (action == 2) {  // UP: release at the last position regardless of geometry
            input.injectTouch(2, pointerId, px, py);
            return;
        }
        if (!conn.streaming || conn.curNatW <= 0) return;  // no geometry yet
        int[] lp = Projection.panelToLogical(px, py, conn.curNatW, conn.curNatH,
                conn.curTargetW, conn.curTargetH, conn.curScaleMode, conn.curRotation);
        if (lp == null) return;  // letterbox tap — no source pixel there
        input.injectTouch(action, pointerId, lp[0], lp[1]);
    }

    /**
     * Build the input injector once, on the MAIN thread (the caller). Must run on a
     * thread with a Looper because {@code new ActivityThread()} creates an internal
     * Handler bound to the current thread. Runs before any connection's reader
     * thread starts, so the plain field is safely published to it.
     */
    void initInput() {
        try {
            input = Input.create();
        } catch (Throwable t) {
            // Unwrap the reflective wrapper so the real cause is visible.
            Throwable cause = (t instanceof java.lang.reflect.InvocationTargetException
                    && t.getCause() != null) ? t.getCause() : t;
            System.err.println("tab5adb-agent: input injection unavailable: " + cause);
            cause.printStackTrace();
        }
    }

    /**
     * Build the app-metadata service once on the MAIN thread (the caller), like
     * {@link #initInput} — SystemContext needs this thread's Looper. Best-effort:
     * on failure the HELLO simply doesn't advertise APPINFO.
     */
    void initAppInfo() {
        try {
            appInfo = AppInfo.create();
        } catch (Throwable t) {
            System.err.println("tab5adb-agent: app info unavailable: " + t);
        }
    }

    // --- GET_APP_LIST / GET_APP_ICON (§4.4) — answered on the reader thread ---

    private void handleGetAppList(Conn conn, int reqId) throws IOException {
        if (appInfo == null) {
            conn.sendControlResponse(CMD_GET_APP_LIST, reqId, STATUS_ENOTSUP, null);
            return;
        }
        try {
            byte[] result = appInfo.appListPayload();
            conn.sendControlResponse(CMD_GET_APP_LIST, reqId, STATUS_OK, result);
            System.out.println("tab5adb-agent: GET_APP_LIST -> " + result.length + "B");
        } catch (Throwable t) {
            System.err.println("tab5adb-agent: GET_APP_LIST failed: " + t);
            conn.sendControlResponse(CMD_GET_APP_LIST, reqId, STATUS_EFAIL, null);
        }
    }

    private void handleGetAppIcon(Conn conn, byte[] p, int reqId) throws IOException {
        if (appInfo == null) {
            conn.sendControlResponse(CMD_GET_APP_ICON, reqId, STATUS_ENOTSUP, null);
            return;
        }
        // args (§4.4): size_px(u16) + reserved(u16) + package (rest of payload).
        if (p.length < 2 + 4 + 1) {
            conn.sendControlResponse(CMD_GET_APP_ICON, reqId, STATUS_EINVAL, null);
            return;
        }
        int size = readU16(p, 2);
        String pkg = new String(p, 6, p.length - 6, java.nio.charset.StandardCharsets.UTF_8);
        if (size < 16 || size > 256) {
            conn.sendControlResponse(CMD_GET_APP_ICON, reqId, STATUS_EINVAL, null);
            return;
        }
        try {
            byte[] result = appInfo.appIconPayload(pkg, size);
            conn.sendControlResponse(CMD_GET_APP_ICON, reqId, STATUS_OK, result);
        } catch (Throwable t) {
            // Unknown package / drawing failure (§4.4).
            System.err.println("tab5adb-agent: GET_APP_ICON " + pkg + " failed: " + t);
            conn.sendControlResponse(CMD_GET_APP_ICON, reqId, STATUS_EINVAL, null);
        }
    }

    // --- HELLO response (§4.4) ---

    private void handleHelloResponse(Conn conn, Frame resp) {
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
        synchronized (conn.lock) {
            conn.helloOk = true;
            conn.lock.notifyAll();
        }
        System.out.println("tab5adb-agent: HELLO ok caps=0x" + Integer.toHexString(caps)
                + " max_payload=" + maxPayload);
    }

    // --- MIRROR_START / MIRROR_STOP (§4.4) ---

    private static final class MirrorParams {
        int targetW, targetH, scaleMode, streams, maxFps, quality, split;
        boolean audio;  // start the AUDIO stream (streams has AUDIO and we can capture)
    }

    /** Parse + answer MIRROR_START; hand the params to the session loop. */
    private void handleMirrorStart(Conn conn, byte[] p, int reqId) throws IOException {
        if (p.length < 2 + 8) throw new IllegalStateException("malformed MIRROR_START");
        MirrorParams mp = new MirrorParams();
        mp.targetW = readU16(p, 2);
        mp.targetH = readU16(p, 4);
        mp.scaleMode = p[6] & 0xFF;
        mp.streams = p[7] & 0xFF;
        // Appended tail (§4.4, append-only): max_fps / jpeg_quality / split_count
        // at +8..+10; absent or 0 = the defaults.
        mp.maxFps = p.length >= 2 + 9 ? (p[10] & 0xFF) : 0;
        mp.quality = p.length >= 2 + 10 ? (p[11] & 0xFF) : 0;
        mp.split = p.length >= 2 + 11 ? (p[12] & 0xFF) : 0;
        System.out.println("tab5adb-agent: MIRROR_START target=" + mp.targetW + "x" + mp.targetH
                + " scale=" + mp.scaleMode + " streams=0x" + Integer.toHexString(mp.streams)
                + " max_fps=" + mp.maxFps + " q=" + mp.quality + " split=" + mp.split);

        // Adapt mode (§5.3): resize the SOURCE itself (wm size) to the viewer's
        // aspect so a plain fit fills it with no letterbox/crop (the source app
        // reflows). We restore the device size on MIRROR_STOP / disconnect / shutdown
        // (serve() + the shutdown hook), so the Tab5 never has to send a reset — the
        // override can't survive a yanked cable. Switching to fit/fill restores first.
        if (mp.scaleMode == SCALE_ADAPT) {
            if (!adaptActive) {
                // Natural-orientation size: `wm size`/setForcedDisplaySize take the
                // unrotated WxH (sourceSize() is the current-rotation size, which would
                // force a landscape size onto a portrait-natural device when rotated).
                int[] nat = naturalSourceSize();
                int[] sz = adaptSize(nat[0], nat[1], mp.targetW, mp.targetH);
                applyAdaptSize(sz[0], sz[1]);
                // Let the resize settle before the response/capture read the new size
                // (only when it actually applied — a failure leaves adaptActive false).
                if (adaptActive) waitForSourceSize(sz[0], sz[1], 600);
            }
            mp.scaleMode = 0;  // source is now viewer-aspect: stream a plain fit
        } else if (adaptActive) {
            clearAdaptSize();  // leaving Adapt for fit/fill: restore the device size
        }

        // Aspect mode (§5.3): size the output to the source's natural aspect within
        // the requested box, then stream a plain fit into that box — a full-bleed
        // frame whose size the response's out_width/out_height carries.
        if (mp.scaleMode == SCALE_ASPECT) {
            int[] nat = naturalSourceSize();
            int[] out = Projection.aspectOutput(nat[0], nat[1], mp.targetW, mp.targetH);
            mp.targetW = out[0];
            mp.targetH = out[1];
            mp.scaleMode = 0;
            System.out.println("tab5adb-agent: aspect output " + out[0] + "x" + out[1]
                    + " (source " + nat[0] + "x" + nat[1] + ")");
        }
        // Publish the panel geometry for touch passthrough (§4.7); the rotation /
        // natural source dims follow once streamVideo builds the capture.
        conn.curTargetW = mp.targetW;
        conn.curTargetH = mp.targetH;
        conn.curScaleMode = mp.scaleMode;

        if ((mp.streams & CAP_VIDEO) == 0) {  // mirror always carries video
            conn.sendControlResponse(CMD_MIRROR_START, reqId, STATUS_ENOTSUP, null);
            return;
        }

        // Audio is started only if requested AND we can capture it (Android 12+ /
        // test-tone); otherwise we proceed video-only and omit the §6.2 audio tail.
        mp.audio = (mp.streams & CAP_AUDIO) != 0 && audioAvailable();

        int[] src = sourceSize();
        byte[] result = new byte[mp.audio ? 20 : 12];
        writeU16(result, 0, src[0]);          // source_width
        writeU16(result, 2, src[1]);          // source_height
        result[4] = (byte) VIDEO_CODEC_JPEG;  // video_codec
        result[5] = 0;                        // reserved
        writeU16(result, 6, 0);               // reserved
        writeU16(result, 8, mp.targetW);      // out_width (== target after the remap)
        writeU16(result, 10, mp.targetH);     // out_height
        if (mp.audio) {  // §6.2 audio tail: PCM_S16LE / stereo / 48 kHz
            result[12] = (byte) AudioCapture.CODEC_PCM_S16LE;  // audio_codec
            result[13] = (byte) AudioCapture.CHANNELS;         // audio_channels
            writeU16(result, 14, 0);                           // reserved
            writeU32(result, 16, AudioCapture.SAMPLE_RATE);    // audio_rate
        }
        conn.sendControlResponse(CMD_MIRROR_START, reqId, STATUS_OK, result);

        // Hand off to the session loop; clear any stale stop from a prior session.
        synchronized (conn.lock) {
            conn.pendingStart = mp;
            conn.stopRequested = false;
            conn.lock.notifyAll();
        }
    }

    /** MIRROR_STOP: signal the stream loop to stop (back to READY), ack OK. */
    private void handleMirrorStop(Conn conn, int reqId) throws IOException {
        System.out.println("tab5adb-agent: MIRROR_STOP");
        synchronized (conn.lock) {
            conn.stopRequested = true;
            conn.lock.notifyAll();
        }
        conn.sendControlResponse(CMD_MIRROR_STOP, reqId, STATUS_OK, null);
    }

    // --- video stream (§5) ---

    private void streamVideo(Conn conn, MirrorParams mp) throws Exception {
        int split = mp.split > 0 ? mp.split : SPLIT_COUNT;
        int quality = mp.quality > 0 ? Math.min(mp.quality, 100) : JPEG_QUALITY;
        FramePipeline pipeline = new FramePipeline(
                mp.targetW, mp.targetH, mp.scaleMode, split, quality);
        System.out.println("tab5adb-agent: streaming video (split=" + split
                + " q=" + quality + (testPattern ? ", test-pattern)" : ", screen)"));

        // Wall-clock pacing: the Tab5's requested max_fps (a preview asks for ~10)
        // bounded by our own panel-rate cap.
        int fps = mp.maxFps > 0 ? Math.min(mp.maxFps, TARGET_FPS) : TARGET_FPS;
        long frameNs = fps > 0 ? 1_000_000_000L / fps : 0;
        int frame = 0;
        ScreenCapture capture = null;
        int captureRotation = -1;  // device rotation the current capture was built for
        conn.streaming = true;     // touch passthrough (§4.7) may now map panel->source
        try {
            // Stream until MIRROR_STOP (-> READY), the peer closes, or a new
            // MIRROR_START arrives (pendingStart != null) — the last is a mode/scale
            // switch: break so the session loop restarts streamVideo with the new mp.
            while (!conn.stopRequested && !conn.closed && conn.pendingStart == null) {
                long t0 = System.nanoTime();
                if (!testPattern) {
                    // The Tab5 is fixed to the device's physical orientation (§5.1), so
                    // the capture undoes the device's logical rotation. Rebuild it when
                    // the device rotates (the reader size + counter-rotation depend on it).
                    int rotation = deviceRotation();
                    if (capture == null || rotation != captureRotation) {
                        if (capture != null) capture.close();
                        int[] src = sourceSize();
                        capture = new ScreenCapture(src[0], src[1], mp.targetW, mp.targetH,
                                mp.scaleMode, rotation);
                        captureRotation = rotation;
                        // Publish geometry for the touch-passthrough inverse (§4.7):
                        // the natural (portrait) source dims + current rotation.
                        int[] nat = Projection.naturalSize(src[0], src[1], rotation);
                        conn.curRotation = rotation;
                        conn.curNatW = nat[0];
                        conn.curNatH = nat[1];
                        System.out.println("tab5adb-agent: capture built for rotation " + rotation);
                        // Tell the Tab5 the device's orientation so it lays the
                        // overlay UI out for portrait vs landscape (§4.4). Sent on
                        // the first frame of every stream and on each rotation change.
                        sendOrientation(conn, rotation);
                    }
                }
                Bitmap src = nextSource(capture, frame);
                if (src == null) {  // capture not ready yet — wait for the first frame
                    sleep(10);
                    continue;
                }
                List<FramePipeline.Strip> strips;
                try {
                    // Real capture is a GPU-geometry frame → strip it (fit: panel-sized,
                    // crop (0,0); fill: oversized cover, crop the centered panel out via
                    // the capture's crop origin). The test pattern is a raw source → run
                    // the CPU pipeline.
                    strips = testPattern ? pipeline.process(src)
                            : pipeline.stripsOf(src, capture.cropX(), capture.cropY());
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
            // write failed = peer closed the stream; let the reader mark it closed
            synchronized (conn.lock) {
                conn.closed = true;
                conn.lock.notifyAll();
            }
        } finally {
            conn.streaming = false;  // stop mapping touches to a torn-down capture
            if (capture != null) capture.close();
        }
        String why = conn.stopRequested ? "stopped"
                : (conn.pendingStart != null ? "reconfiguring" : "ended");
        System.out.println("tab5adb-agent: stream " + why + " after " + frame + " frames");
    }

    private Bitmap nextSource(ScreenCapture capture, int frame) {
        if (testPattern) return TestPattern.make(testW, testH, frame);
        return capture.acquire();
    }

    /** Notify the Tab5 of the source device's logical rotation (ORIENTATION, §4.4). */
    private void sendOrientation(Conn conn, int rotation) throws IOException {
        byte[] payload = new byte[1 + 4];  // event + rotation + 3 reserved
        payload[0] = (byte) EVENT_ORIENTATION;
        payload[1] = (byte) rotation;      // Surface.ROTATION_* (0..3)
        conn.writeFrame(TYPE_EVENT, 0, payload);
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

    // --- audio stream (§6) — captured + sent on its own thread, parallel to video ---

    /** Whether we can offer the AUDIO stream: Android 12+ for real capture, or test-tone. */
    private boolean audioAvailable() {
        return testTone || android.os.Build.VERSION.SDK_INT >= 31;  // Android 12+ (§6)
    }

    /**
     * Captures device audio ({@link AudioCapture}) and writes it as TYPE=AUDIO frames
     * on a dedicated thread, so audio never blocks the JPEG flow and vice versa
     * (frame writes serialize in {@link Conn#writeFrame}). Each chunk is one
     * self-contained PCM frame (FRAME_START|FRAME_END, §6.3). Stops when streaming
     * ends — the session loop {@link #stop}s it after {@code streamVideo} returns,
     * and the loop also exits on the same {@code conn} flags.
     */
    private final class AudioStreamer {
        private final Conn conn;
        private final AudioCapture cap;
        private final Thread thread;
        private volatile boolean running = true;

        AudioStreamer(Conn conn) {
            this.conn = conn;
            this.cap = new AudioCapture(testTone);
            this.thread = new Thread(this::loop, "tab5adb-audio");
            this.thread.setDaemon(true);
        }

        /** Open capture (may throw) then start the send thread. */
        void start() {
            cap.start();
            thread.start();
        }

        private void loop() {
            byte[] buf = new byte[AudioCapture.CHUNK_BYTES];
            long chunks = 0;
            try {
                while (running && !conn.closed && !conn.stopRequested
                        && conn.pendingStart == null) {
                    int n = cap.read(buf);
                    if (n <= 0) break;
                    // writeFrame is synchronous, so reusing `buf` next iteration is safe;
                    // copy only the short last chunk.
                    byte[] payload = (n == buf.length) ? buf : java.util.Arrays.copyOf(buf, n);
                    conn.writeFrame(TYPE_AUDIO, FLAG_FRAME_START | FLAG_FRAME_END, payload);
                    chunks++;
                }
            } catch (IOException eof) {
                synchronized (conn.lock) {
                    conn.closed = true;
                    conn.lock.notifyAll();
                }
            } catch (Throwable t) {
                System.err.println("tab5adb-agent: audio: " + t);
            } finally {
                cap.close();
            }
            System.out.println("tab5adb-agent: audio stopped after " + chunks + " chunks");
        }

        /** Stop the send thread and release capture (idempotent; unblocks read()). */
        void stop() {
            running = false;
            cap.close();
            try {
                thread.join(500);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
    }

    // --- per-connection framing (§3) ---

    private static final class Conn {
        final OutputStream out;
        final DataInputStream in;
        int outSeq = 0;                 // outgoing frame counter (§3 SEQ), per direction
        volatile long maxPayload = 0;   // Tab5's accepted payload cap (from HELLO)

        // Session state shared between the reader thread and the main (stream)
        // thread. `lock` guards the wait/notify handoff; the volatile flags are
        // also read in the stream loop without the lock.
        final Object lock = new Object();
        volatile boolean helloOk = false;     // HELLO response received + accepted
        volatile boolean closed = false;      // peer disconnected / fatal error
        volatile boolean stopRequested = false;  // MIRROR_STOP for the live stream
        // MIRROR_START to (re)start. Assigned/consumed under `lock`, but ALSO read
        // lock-free in the stream loop so a MIRROR_START arriving mid-stream makes the
        // current stream break and the session loop restart with the new params
        // (mode switch without a stop) — hence volatile.
        volatile MirrorParams pendingStart = null;

        // Live mirror geometry, for the touch-passthrough inverse mapping (§4.7).
        // Written by the stream thread (handleMirrorStart + streamVideo), read by
        // the reader thread (handleTouch); volatile is enough (no cross-field
        // invariant the reader depends on atomically).
        volatile boolean streaming = false;
        volatile int curTargetW = 0, curTargetH = 0, curScaleMode = 0;
        volatile int curRotation = 0;        // Surface.ROTATION_* the capture is built for
        volatile int curNatW = 0, curNatH = 0;  // natural source dims (0 = not ready)

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

        // Serialized: the reader thread (control responses) and the main thread
        // (HELLO request + JPEG frames) both write, so a whole frame is emitted
        // atomically and SEQ stays consistent.
        synchronized void writeFrame(int type, int flags, byte[] payload) throws IOException {
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

    /**
     * The source's dimensions as the pipeline will display them, for the aspect
     * scale mode's output sizing. Real capture shows the natural-orientation
     * framebuffer (§5.1), so un-rotate the logical size; the test-pattern CPU
     * pipeline rotates landscape sources to portrait, so portrait-ize those.
     */
    private int[] naturalSourceSize() {
        if (testPattern) {
            return testW > testH ? new int[]{testH, testW} : new int[]{testW, testH};
        }
        int[] src = sourceSize();
        return Projection.naturalSize(src[0], src[1], deviceRotation());
    }

    /**
     * Display 0's current logical rotation as a {@code Surface.ROTATION_*} code
     * (0/1/2/3). {@link ScreenCapture} undoes this so the Tab5 stays fixed to the
     * device's physical orientation (§5.1). Best-effort: treat a failure as ROTATION_0.
     */
    private int deviceRotation() {
        try {
            Class<?> dmg = Class.forName("android.hardware.display.DisplayManagerGlobal");
            Object inst = dmg.getMethod("getInstance").invoke(null);
            Object display = dmg.getMethod("getRealDisplay", int.class).invoke(inst, 0);
            return (int) display.getClass().getMethod("getRotation").invoke(display);
        } catch (Throwable t) {
            System.err.println("tab5adb-agent: getRotation failed, assuming 0: " + t);
            return 0;
        }
    }

    // --- Adapt scale mode (§5.3 kScaleAdapt) ----------------------------------
    // Resize the SOURCE display (wm size, via IWindowManager) to the viewer's aspect
    // so a plain fit fills it with no letterbox/crop. The agent owns the restore so a
    // disconnect (yanked cable) can't leave the device stuck at the override: it is
    // cleared on MIRROR_STOP / reconfigure-away / disconnect (serve()) and on JVM exit
    // (the shutdown hook in main, covering SIGTERM/SIGHUP — not SIGKILL).

    private final Object adaptLock = new Object();
    private volatile boolean adaptActive = false;  // a forced display size is applied

    /** Override size for a physW×physH source shown in a targetW×targetH viewer: keep
     *  the device's short side, stretch the long side to the viewer aspect (portrait
     *  vs landscape preserved). Mirrors the Tab5's old compute_adapt_size. */
    private static int[] adaptSize(int physW, int physH, int targetW, int targetH) {
        int shortSide = Math.min(physW, physH);
        int tLong = Math.max(targetW, targetH), tShort = Math.min(targetW, targetH);
        int longSide = (int) ((long) shortSide * tLong / tShort);
        return physW <= physH ? new int[]{physW, longSide}    // portrait: keep width
                              : new int[]{longSide, physH};   // landscape: keep height
    }

    /** IWindowManager binder (shell uid reaches it, the same privilege `wm` uses). */
    private Object windowManager() throws Exception {
        Class<?> sm = Class.forName("android.os.ServiceManager");
        Class<?> ibinder = Class.forName("android.os.IBinder");
        Object binder = sm.getMethod("getService", String.class).invoke(null, "window");
        Class<?> stub = Class.forName("android.view.IWindowManager$Stub");
        return stub.getMethod("asInterface", ibinder).invoke(null, binder);
    }

    /** Force display 0's size (= `wm size WxH`). */
    private void applyAdaptSize(int w, int h) {
        synchronized (adaptLock) {
            try {
                Object wm = windowManager();
                Class.forName("android.view.IWindowManager")
                        .getMethod("setForcedDisplaySize", int.class, int.class, int.class)
                        .invoke(wm, 0, w, h);
                adaptActive = true;
                System.out.println("tab5adb-agent: adapt wm size " + w + "x" + h);
            } catch (Throwable t) {
                System.err.println("tab5adb-agent: adapt setForcedDisplaySize failed: " + t);
            }
        }
    }

    /** Restore display 0's real size (= `wm size reset`); no-op if not overridden. */
    private void clearAdaptSize() {
        synchronized (adaptLock) {
            if (!adaptActive) return;
            try {
                Object wm = windowManager();
                Class.forName("android.view.IWindowManager")
                        .getMethod("clearForcedDisplaySize", int.class).invoke(wm, 0);
                System.out.println("tab5adb-agent: adapt wm size reset");
            } catch (Throwable t) {
                System.err.println("tab5adb-agent: adapt clearForcedDisplaySize failed: " + t);
            }
            adaptActive = false;
        }
    }

    /** Block (briefly) until sourceSize() reflects the forced size, so the capture is
     *  built at the new resolution rather than the stale physical one. Compares the
     *  dimensions as an unordered set — sourceSize() is rotation-dependent while the
     *  forced (w,h) is natural-orientation, so the two may be transposed. */
    private void waitForSourceSize(int w, int h, int timeoutMs) {
        int wantLo = Math.min(w, h), wantHi = Math.max(w, h);
        long deadline = System.currentTimeMillis() + timeoutMs;
        while (System.currentTimeMillis() < deadline) {
            try {
                int[] s = sourceSize();
                if (Math.min(s[0], s[1]) == wantLo && Math.max(s[0], s[1]) == wantHi) return;
            } catch (Throwable ignored) { /* transient during reconfigure */ }
            try { Thread.sleep(30); } catch (InterruptedException e) { return; }
        }
    }
}
