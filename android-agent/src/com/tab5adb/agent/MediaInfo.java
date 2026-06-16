package com.tab5adb.agent;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.media.MediaMetadata;
import android.media.session.MediaController;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;

import java.io.ByteArrayOutputStream;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.List;

/**
 * Now-playing media for the Tab5 DeviceScreen card (protocol.md §4.4 MEDIA event /
 * GET_MEDIA_INFO / GET_MEDIA_RENDER / MEDIA_CONTROL): the active session's state,
 * album art (raw ARGB8888), agent-rendered title/artist (A8 — so the Tab5 needs no
 * CJK fonts), and transport control.
 *
 * <p>app_process's {@code getSystemService("media_session")} NPEs (the media
 * framework initializer never ran), so we reach the data over the binder the way
 * {@code cmd media_session} does: {@code ISessionManager.getSessions(null, userId,
 * deviceId)} (shell uid holds MEDIA_CONTENT_CONTROL) → a {@link MediaController}
 * per token (public ctor). Metadata / playback / art / transport controls /
 * {@link MediaController#registerCallback} all work off that controller.
 *
 * <p>Changes push as MEDIA events (real-time): a {@link MediaController.Callback}
 * fires instantly on metadata/playback change, and a periodic rescan on the
 * monitor thread detects session-set changes (a different app starting playback).
 * The Tab5 re-fetches art/text only when {@code content_token} changes.
 */
final class MediaInfo {
    /** Sink for agent-initiated MEDIA event frames; set per-connection by the Server. */
    interface Sink {
        void send(byte[] eventData);
    }

    // Wire state codes (protocol.md MEDIA event).
    private static final int ST_NONE = 0, ST_PLAYING = 1, ST_PAUSED = 2,
            ST_BUFFERING = 3, ST_STOPPED = 4;

    // Render section kinds / formats.
    private static final int KIND_ART = 0, KIND_TITLE = 1, KIND_ARTIST = 2;
    private static final int FMT_ABSENT = 0, FMT_ARGB8888 = 1, FMT_A8 = 2;

    private static final long RESCAN_MS = 1500;

    private final Context ctx;
    private final Object sessionManager;       // ISessionManager
    private final Method getSessions;          // (ComponentName, int, int) -> List<Token>
    private final TextRender text;             // null if no usable font (text -> absent)
    private final HandlerThread thread;
    private final Handler handler;

    private volatile Sink sink;
    private volatile MediaController current;  // active controller (or null)
    private MediaSession.Token currentToken;   // monitor-thread only
    private MediaController.Callback callback;  // monitor-thread only

    // Last pushed snapshot, for change detection (monitor thread).
    private int lastState = -1;
    private int lastHasArt = -1;
    private long lastToken = -1;

    private MediaInfo(Context ctx, Object sessionManager, Method getSessions, TextRender text) {
        this.ctx = ctx;
        this.sessionManager = sessionManager;
        this.getSessions = getSessions;
        this.text = text;
        this.thread = new HandlerThread("tab5adb-media");
        this.thread.start();
        this.handler = new Handler(thread.getLooper());
    }

    /** Build the service; throws if the session binder is unreachable. */
    static MediaInfo create() throws Exception {
        Context ctx = SystemContext.get();
        Class<?> smClass = Class.forName("android.os.ServiceManager");
        IBinder binder = (IBinder) smClass.getMethod("getService", String.class)
                .invoke(null, "media_session");
        if (binder == null) throw new IllegalStateException("no media_session binder");
        Class<?> stub = Class.forName("android.media.session.ISessionManager$Stub");
        Object sm = stub.getMethod("asInterface", IBinder.class).invoke(null, binder);
        Method gs = null;
        for (Method m : sm.getClass().getMethods()) {
            if (m.getName().equals("getSessions")) { gs = m; break; }
        }
        if (gs == null) throw new IllegalStateException("no ISessionManager.getSessions");

        TextRender tr = null;
        try {
            tr = TextRender.create();
        } catch (Throwable t) {
            System.err.println("tab5adb-agent: media text render unavailable: " + t);
        }
        MediaInfo mi = new MediaInfo(ctx, sm, gs, tr);
        mi.handler.post(mi::rescanAndSchedule);
        return mi;
    }

    /** Set/clear the event sink (per connection); pushes the current state at once. */
    void setSink(Sink s) {
        this.sink = s;
        // Reset the change-detection baseline so the first push always goes out.
        handler.post(() -> {
            lastState = lastHasArt = -1;
            lastToken = -1;
            maybePush();
        });
    }

    // --- monitor thread: pick the active session, watch it, detect changes ------

    private void rescanAndSchedule() {
        try {
            rescan();
        } catch (Throwable t) {
            System.err.println("tab5adb-agent: media rescan: " + t);
        }
        handler.postDelayed(this::rescanAndSchedule, RESCAN_MS);
    }

    private void rescan() throws Exception {
        MediaController chosen = pickController();
        MediaSession.Token tok = chosen == null ? null : chosen.getSessionToken();
        boolean changed = (tok == null) != (currentToken == null)
                || (tok != null && !tok.equals(currentToken));
        if (changed) {
            if (current != null && callback != null) {
                try { current.unregisterCallback(callback); } catch (Throwable ignore) {}
            }
            current = chosen;
            currentToken = tok;
            if (chosen != null) {
                callback = new MediaController.Callback() {
                    @Override public void onSessionDestroyed() { handler.post(MediaInfo.this::onDestroyed); }
                    @Override public void onPlaybackStateChanged(PlaybackState s) { maybePush(); }
                    @Override public void onMetadataChanged(MediaMetadata m) { maybePush(); }
                };
                try { chosen.registerCallback(callback, handler); } catch (Throwable ignore) {}
            } else {
                callback = null;
            }
        }
        maybePush();
    }

    private void onDestroyed() {
        current = null;
        currentToken = null;
        callback = null;
        try { rescan(); } catch (Throwable ignore) {}
    }

    /** Highest-ranked session with metadata + a real state (Playing>Buffering>Paused). */
    private MediaController pickController() throws Exception {
        @SuppressWarnings("unchecked")
        List<MediaSession.Token> tokens =
                (List<MediaSession.Token>) getSessions.invoke(sessionManager, callArgs());
        MediaController best = null;
        int bestRank = 0;
        for (MediaSession.Token t : tokens) {
            MediaController mc;
            try { mc = new MediaController(ctx, t); } catch (Throwable ex) { continue; }
            if (mc.getMetadata() == null) continue;
            int rank = rankOf(mc.getPlaybackState());
            if (rank > bestRank) { best = mc; bestRank = rank; if (rank == 3) break; }
        }
        return best;
    }

    /** getSessions params vary (ComponentName, [int userId,] [int deviceId]); fill ints with 0. */
    private Object[] callArgs() {
        Class<?>[] pt = getSessions.getParameterTypes();
        Object[] a = new Object[pt.length];
        for (int i = 0; i < pt.length; i++) a[i] = (pt[i] == int.class) ? Integer.valueOf(0) : null;
        return a;
    }

    private static int rankOf(PlaybackState ps) {
        if (ps == null) return 0;
        switch (ps.getState()) {
            case PlaybackState.STATE_PLAYING: return 3;
            case PlaybackState.STATE_BUFFERING: return 2;
            case PlaybackState.STATE_PAUSED: return 1;
            default: return 0;
        }
    }

    private static int wireState(PlaybackState ps) {
        if (ps == null) return ST_NONE;
        switch (ps.getState()) {
            case PlaybackState.STATE_PLAYING: return ST_PLAYING;
            case PlaybackState.STATE_PAUSED: return ST_PAUSED;
            case PlaybackState.STATE_BUFFERING: return ST_BUFFERING;
            case PlaybackState.STATE_STOPPED: return ST_STOPPED;
            default: return ST_NONE;
        }
    }

    // --- snapshot / token -------------------------------------------------------

    private static String txt(MediaMetadata m, String key) {
        CharSequence c = m.getText(key);
        return c == null ? "" : c.toString();
    }

    /** content_token = stable hash of the track identity (NOT the play state). */
    private static long tokenOf(MediaController mc) {
        MediaMetadata m = mc == null ? null : mc.getMetadata();
        if (m == null) return 0;
        String id = txt(m, MediaMetadata.METADATA_KEY_TITLE) + ""
                + txt(m, MediaMetadata.METADATA_KEY_ARTIST) + ""
                + txt(m, MediaMetadata.METADATA_KEY_ALBUM);
        int h = id.hashCode();
        return (h & 0xFFFFFFFFL);
    }

    private static boolean hasArt(MediaController mc) {
        MediaMetadata m = mc == null ? null : mc.getMetadata();
        return m != null && artBitmap(m) != null;
    }

    private static Bitmap artBitmap(MediaMetadata m) {
        Bitmap b = m.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART);
        if (b == null) b = m.getBitmap(MediaMetadata.METADATA_KEY_ART);
        if (b == null) b = m.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON);
        return b;
    }

    /** 8-byte MEDIA event / GET_MEDIA_INFO payload from the current controller. */
    byte[] snapshotPayload() {
        MediaController mc = current;
        int state = mc == null ? ST_NONE : wireState(mc.getPlaybackState());
        long token = tokenOf(mc);
        int hasArt = (token != 0 && hasArt(mc)) ? 1 : 0;
        byte[] p = new byte[8];
        p[0] = (byte) state;
        p[1] = (byte) hasArt;
        // p[2..3] reserved
        writeU32(p, 4, token);
        return p;
    }

    private void maybePush() {
        Sink s = sink;
        if (s == null) return;
        MediaController mc = current;
        int state = mc == null ? ST_NONE : wireState(mc.getPlaybackState());
        long token = tokenOf(mc);
        int hasArt = (token != 0 && hasArt(mc)) ? 1 : 0;
        if (state == lastState && hasArt == lastHasArt && token == lastToken) return;
        lastState = state; lastHasArt = hasArt; lastToken = token;
        byte[] p = new byte[8];
        p[0] = (byte) state;
        p[1] = (byte) hasArt;
        writeU32(p, 4, token);
        s.send(p);
    }

    // --- render (album art ARGB8888 + title/artist A8) --------------------------

    byte[] renderPayload(int width, int artPx, int titlePx, int artistPx) {
        MediaController mc = current;
        MediaMetadata m = mc == null ? null : mc.getMetadata();
        ByteArrayOutputStream out = new ByteArrayOutputStream(artPx * artPx * 4 + 8192);
        out.write(3);            // section_count (art, title, artist)
        out.write(0);            // reserved
        out.write(0); out.write(0);

        // ART
        Bitmap art = (m != null && artPx > 0) ? artBitmap(m) : null;
        if (art != null) {
            try { writeArt(out, art, artPx); }
            catch (Throwable t) { writeAbsent(out, KIND_ART); }
        } else {
            writeAbsent(out, KIND_ART);
        }
        // TITLE / ARTIST
        writeTextSection(out, KIND_TITLE, m == null ? "" : txt(m, MediaMetadata.METADATA_KEY_TITLE),
                titlePx, width);
        writeTextSection(out, KIND_ARTIST, m == null ? "" : txt(m, MediaMetadata.METADATA_KEY_ARTIST),
                artistPx, width);
        return out.toByteArray();
    }

    private void writeArt(ByteArrayOutputStream out, Bitmap art, int artPx) {
        // Draw onto an ARGB_8888 canvas (handles any source config — incl. a
        // HARDWARE bitmap, which getPixels can't read directly) then read back.
        Bitmap dst = Bitmap.createBitmap(artPx, artPx, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(dst);
        Paint p = new Paint(Paint.FILTER_BITMAP_FLAG);
        c.drawBitmap(art, new Rect(0, 0, art.getWidth(), art.getHeight()),
                new Rect(0, 0, artPx, artPx), p);
        int[] px = new int[artPx * artPx];
        dst.getPixels(px, 0, artPx, 0, 0, artPx, artPx);
        dst.recycle();
        sectionHeader(out, KIND_ART, FMT_ARGB8888, artPx, artPx, px.length * 4);
        for (int v : px) {  // Color int 0xAARRGGBB -> LE bytes B,G,R,A
            out.write(v & 0xFF);
            out.write((v >> 8) & 0xFF);
            out.write((v >> 16) & 0xFF);
            out.write((v >> 24) & 0xFF);
        }
    }

    private void writeTextSection(ByteArrayOutputStream out, int kind, String s, int px, int width) {
        TextRender.Line line = (text == null) ? null : text.render(s, px, width);
        if (line == null) { writeAbsent(out, kind); return; }
        sectionHeader(out, kind, FMT_A8, line.w, line.h, line.alpha.length);
        out.write(line.alpha, 0, line.alpha.length);
    }

    private static void writeAbsent(ByteArrayOutputStream out, int kind) {
        sectionHeader(out, kind, FMT_ABSENT, 0, 0, 0);
    }

    private static void sectionHeader(ByteArrayOutputStream out, int kind, int fmt,
                                      int w, int h, int dataLen) {
        out.write(kind);
        out.write(fmt);
        out.write(w & 0xFF); out.write((w >> 8) & 0xFF);
        out.write(h & 0xFF); out.write((h >> 8) & 0xFF);
        out.write(dataLen & 0xFF); out.write((dataLen >> 8) & 0xFF);
        out.write((dataLen >> 16) & 0xFF); out.write((dataLen >> 24) & 0xFF);
    }

    // --- transport control ------------------------------------------------------

    /** Dispatch a transport action (0=play_pause,1=next,2=previous,3=play,4=pause). */
    boolean control(int action) {
        MediaController mc = current;
        if (mc == null) return false;
        MediaController.TransportControls tc = mc.getTransportControls();
        try {
            switch (action) {
                case 1: tc.skipToNext(); return true;
                case 2: tc.skipToPrevious(); return true;
                case 3: tc.play(); return true;
                case 4: tc.pause(); return true;
                case 0:
                default: {
                    PlaybackState ps = mc.getPlaybackState();
                    boolean playing = ps != null && ps.getState() == PlaybackState.STATE_PLAYING;
                    if (playing) tc.pause(); else tc.play();
                    return true;
                }
            }
        } catch (Throwable t) {
            System.err.println("tab5adb-agent: media control " + action + ": " + t);
            return false;
        }
    }

    private static void writeU32(byte[] b, int off, long v) {
        b[off] = (byte) v;
        b[off + 1] = (byte) (v >> 8);
        b[off + 2] = (byte) (v >> 16);
        b[off + 3] = (byte) (v >> 24);
    }
}
