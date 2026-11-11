/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import android.content.Context;
import android.content.pm.PackageManager;
import android.media.AudioManager;
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
import android.os.SystemClock;

import java.io.ByteArrayOutputStream;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

// The playing media session: MEDIA_STATE, GET_MEDIA_RENDER and MEDIA_CONTROL.
final class MediaInfo {
    interface Sink {
        void send(byte[] state);
    }

    private static final int STATE_NONE = 0;
    private static final int STATE_PLAYING = 1;
    private static final int STATE_PAUSED = 2;
    private static final int STATE_BUFFERING = 3;
    private static final int STATE_STOPPED = 4;
    private static final int FLAG_ART = 1;
    private static final int FLAG_SEEK = 2;
    private static final int FLAG_MUTED = 4;
    private static final int SUBTITLE_ARTIST = 1;
    private static final int SUBTITLE_ALBUM = 2;
    private static final int SUBTITLE_APP = 4;
    private static final int ART_QUALITY = 80;
    private static final int ACTION_PLAY_PAUSE = 0;
    private static final int ACTION_NEXT = 1;
    private static final int ACTION_PREVIOUS = 2;
    private static final int ACTION_SEEK = 3;
    private static final int ACTION_VOLUME = 4;
    private static final int ACTION_MUTE = 5;
    private static final long RESCAN_MS = 1500;
    private static final long JUMP_MS = 1000;
    private static final long VOLUME_POLL_MS = 100;
    private static final String SEPARATOR = " · ";

    private final Context context;
    private final Object sessions;
    private final Method getSessions;
    private final TextRender text;
    private final AudioManager audio;
    private final Handler handler;
    private volatile MediaController current;
    private MediaController.Callback callback;
    private volatile Sink sink;
    private int sentState = -1;
    private int sentVolume;
    private int sentFlags;
    private long sentToken;
    private int sentSpeed;
    private long sentPosition;
    private long sentAt;

    private MediaInfo(Context context, Object sessions, Method getSessions, TextRender text) {
        this.context = context;
        this.sessions = sessions;
        this.getSessions = getSessions;
        this.text = text;
        this.audio = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        HandlerThread thread = new HandlerThread("media");
        thread.start();
        handler = new Handler(thread.getLooper());
    }

    static MediaInfo create(TextRender text) throws Exception {
        Class<?> serviceManager = Class.forName("android.os.ServiceManager");
        IBinder binder = (IBinder) serviceManager.getMethod("getService", String.class).invoke(null, "media_session");
        if (binder == null) {
            throw new IllegalStateException("no media_session");
        }
        Object sessions = Class.forName("android.media.session.ISessionManager$Stub").getMethod("asInterface", IBinder.class).invoke(null, binder);
        Method getSessions = null;
        for (Method method : sessions.getClass().getMethods()) {
            if (method.getName().equals("getSessions")) {
                getSessions = method;
                break;
            }
        }
        if (getSessions == null) {
            throw new IllegalStateException("no ISessionManager.getSessions");
        }
        MediaInfo media = new MediaInfo(SystemContext.get(), sessions, getSessions, text);
        media.handler.post(media::rescan);
        media.handler.post(media::pollVolume);
        return media;
    }

    // `sink` gets the state at once, then every change.
    void setSink(Sink sink) {
        this.sink = sink;
        handler.post(() -> {
            sentState = -1;
            push();
        });
    }

    byte[] state() {
        return encode(current, volume());
    }

    byte[] render(int titlePx, int subtitlePx, int artPx, int subtitle) {
        MediaController controller = current;
        MediaMetadata metadata = controller != null ? controller.getMetadata() : null;
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        text.write(out, metadata != null ? text(metadata, MediaMetadata.METADATA_KEY_TITLE) : "", titlePx);
        List<String> parts = new ArrayList<>();
        if (metadata != null && (subtitle & SUBTITLE_ARTIST) != 0) {
            parts.add(text(metadata, MediaMetadata.METADATA_KEY_ARTIST));
        }
        if (metadata != null && (subtitle & SUBTITLE_ALBUM) != 0) {
            parts.add(text(metadata, MediaMetadata.METADATA_KEY_ALBUM));
        }
        if (controller != null && (subtitle & SUBTITLE_APP) != 0) {
            parts.add(appName(controller.getPackageName()));
        }
        StringBuilder line = new StringBuilder();
        for (String part : parts) {
            if (part.isEmpty()) {
                continue;
            }
            if (line.length() > 0) {
                line.append(SEPARATOR);
            }
            line.append(part);
        }
        text.write(out, line.toString(), subtitlePx);
        Bitmap art = metadata != null && artPx > 0 ? art(metadata) : null;
        byte[] jpeg = art != null ? crop(art, artPx) : new byte[0];
        int size = jpeg.length > 0 ? artPx : 0;
        out.write(size);
        out.write(size >> 8);
        out.write(size);
        out.write(size >> 8);
        for (int shift = 0; shift < 32; shift += 8) {
            out.write(jpeg.length >> shift);
        }
        out.write(jpeg, 0, jpeg.length);
        return out.toByteArray();
    }

    // False when there is no session, or it cannot seek.
    boolean control(int action, long value) {
        if (action == ACTION_MUTE) {
            audio.adjustStreamVolume(AudioManager.STREAM_MUSIC, value != 0 ? AudioManager.ADJUST_MUTE : AudioManager.ADJUST_UNMUTE, 0);
            handler.post(this::push);
            return true;
        }
        if (action == ACTION_VOLUME) {
            if (audio.isStreamMute(AudioManager.STREAM_MUSIC)) {
                audio.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_UNMUTE, 0);
            }
            audio.setStreamVolume(AudioManager.STREAM_MUSIC, (int) Math.min(value, audio.getStreamMaxVolume(AudioManager.STREAM_MUSIC)), 0);
            handler.post(this::push);
            return true;
        }
        MediaController controller = current;
        if (controller == null) {
            return false;
        }
        MediaController.TransportControls controls = controller.getTransportControls();
        PlaybackState state = controller.getPlaybackState();
        switch (action) {
            case ACTION_PLAY_PAUSE:
                if (state != null && state.getState() == PlaybackState.STATE_PLAYING) {
                    controls.pause();
                } else {
                    controls.play();
                }
                return true;
            case ACTION_NEXT:
                controls.skipToNext();
                return true;
            case ACTION_PREVIOUS:
                controls.skipToPrevious();
                return true;
            case ACTION_SEEK:
                if (!canSeek(state)) {
                    return false;
                }
                controls.seekTo(value);
                return true;
            default:
                return false;
        }
    }

    private void pollVolume() {
        push();
        handler.postDelayed(this::pollVolume, VOLUME_POLL_MS);
    }

    // Index, maximum and muted, as encode() lays them out.
    private int volume() {
        int muted = audio.isStreamMute(AudioManager.STREAM_MUSIC) ? 1 : 0;
        return audio.getStreamVolume(AudioManager.STREAM_MUSIC) | audio.getStreamMaxVolume(AudioManager.STREAM_MUSIC) << 8 | muted << 16;
    }

    private void rescan() {
        try {
            choose();
        } catch (Throwable t) {
            System.out.println("media: " + t);
        }
        push();
        handler.postDelayed(this::rescan, RESCAN_MS);
    }

    private void choose() throws Exception {
        MediaController best = null;
        int bestRank = 0;
        @SuppressWarnings("unchecked")
        List<MediaSession.Token> tokens = (List<MediaSession.Token>) getSessions.invoke(sessions, sessionArgs());
        for (MediaSession.Token token : tokens) {
            MediaController controller = new MediaController(context, token);
            int rank = controller.getMetadata() != null ? rank(controller.getPlaybackState()) : 0;
            if (rank > bestRank) {
                best = controller;
                bestRank = rank;
            }
        }
        MediaController old = current;
        if (best == null ? old == null : old != null && best.getSessionToken().equals(old.getSessionToken())) {
            return;
        }
        if (old != null && callback != null) {
            old.unregisterCallback(callback);
        }
        current = best;
        callback = null;
        if (best != null) {
            callback = new MediaController.Callback() {
                @Override
                public void onSessionDestroyed() {
                    current = null;
                    push();
                }

                @Override
                public void onPlaybackStateChanged(PlaybackState state) {
                    push();
                }

                @Override
                public void onMetadataChanged(MediaMetadata metadata) {
                    push();
                }
            };
            best.registerCallback(callback, handler);
        }
    }

    // getSessions takes (ComponentName, int userId[, int deviceId]) depending on the release.
    private Object[] sessionArgs() {
        Class<?>[] types = getSessions.getParameterTypes();
        Object[] args = new Object[types.length];
        for (int i = 0; i < types.length; i++) {
            args[i] = types[i] == int.class ? (Object) 0 : null;
        }
        return args;
    }

    private void push() {
        Sink target = sink;
        if (target == null) {
            return;
        }
        MediaController controller = current;
        PlaybackState playback = controller != null ? controller.getPlaybackState() : null;
        int state = controller != null ? state(playback) : STATE_NONE;
        int flags = flags(controller, playback);
        long token = token(controller);
        int speed = speed(playback);
        long position = position(playback);
        int volume = volume();
        long now = SystemClock.elapsedRealtime();
        long expected = sentPosition + (now - sentAt) * sentSpeed / 1000;
        if (state == sentState && flags == sentFlags && token == sentToken && speed == sentSpeed && volume == sentVolume && Math.abs(position - expected) < JUMP_MS) {
            return;
        }
        sentState = state;
        sentFlags = flags;
        sentToken = token;
        sentSpeed = speed;
        sentVolume = volume;
        sentPosition = position;
        sentAt = now;
        target.send(encode(controller, volume));
    }

    private static byte[] encode(MediaController controller, int volume) {
        PlaybackState playback = controller != null ? controller.getPlaybackState() : null;
        MediaMetadata metadata = controller != null ? controller.getMetadata() : null;
        long duration = metadata != null ? Math.max(0, metadata.getLong(MediaMetadata.METADATA_KEY_DURATION)) : 0;
        byte[] out = new byte[20];
        out[0] = (byte) (controller != null ? state(playback) : STATE_NONE);
        out[1] = (byte) (flags(controller, playback) | ((volume >> 16 & 1) != 0 ? FLAG_MUTED : 0));
        u32(out, 4, token(controller));
        u32(out, 8, position(playback));
        u32(out, 12, duration);
        int speed = speed(playback);
        out[16] = (byte) speed;
        out[17] = (byte) (speed >> 8);
        out[18] = (byte) volume;
        out[19] = (byte) (volume >> 8);
        return out;
    }

    private static int rank(PlaybackState state) {
        if (state == null) {
            return 0;
        }
        switch (state.getState()) {
            case PlaybackState.STATE_PLAYING:
                return 3;
            case PlaybackState.STATE_BUFFERING:
                return 2;
            case PlaybackState.STATE_PAUSED:
                return 1;
            default:
                return 0;
        }
    }

    private static int state(PlaybackState state) {
        if (state == null) {
            return STATE_NONE;
        }
        switch (state.getState()) {
            case PlaybackState.STATE_PLAYING:
                return STATE_PLAYING;
            case PlaybackState.STATE_PAUSED:
                return STATE_PAUSED;
            case PlaybackState.STATE_BUFFERING:
                return STATE_BUFFERING;
            case PlaybackState.STATE_STOPPED:
                return STATE_STOPPED;
            default:
                return STATE_NONE;
        }
    }

    private static int flags(MediaController controller, PlaybackState playback) {
        MediaMetadata metadata = controller != null ? controller.getMetadata() : null;
        int flags = metadata != null && art(metadata) != null ? FLAG_ART : 0;
        return flags | (canSeek(playback) ? FLAG_SEEK : 0);
    }

    private static boolean canSeek(PlaybackState state) {
        return state != null && (state.getActions() & PlaybackState.ACTION_SEEK_TO) != 0;
    }

    private static int speed(PlaybackState state) {
        if (state == null || state.getState() != PlaybackState.STATE_PLAYING) {
            return 0;
        }
        return Math.max(0, Math.min(0xFFFF, Math.round(state.getPlaybackSpeed() * 1000)));
    }

    private static long position(PlaybackState state) {
        if (state == null) {
            return 0;
        }
        long position = state.getPosition();
        if (state.getState() == PlaybackState.STATE_PLAYING && state.getLastPositionUpdateTime() > 0) {
            position += (long) ((SystemClock.elapsedRealtime() - state.getLastPositionUpdateTime()) * state.getPlaybackSpeed());
        }
        return Math.max(0, position);
    }

    private static long token(MediaController controller) {
        MediaMetadata metadata = controller != null ? controller.getMetadata() : null;
        if (metadata == null) {
            return 0;
        }
        String key = text(metadata, MediaMetadata.METADATA_KEY_TITLE) + '\n' + text(metadata, MediaMetadata.METADATA_KEY_ARTIST) + '\n' + text(metadata, MediaMetadata.METADATA_KEY_ALBUM) + '\n' + controller.getPackageName();
        int hash = key.hashCode();
        return hash != 0 ? hash & 0xFFFFFFFFL : 1;
    }

    private static String text(MediaMetadata metadata, String key) {
        CharSequence value = metadata.getText(key);
        return value != null ? value.toString().trim() : "";
    }

    private static Bitmap art(MediaMetadata metadata) {
        Bitmap art = metadata.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART);
        if (art == null) {
            art = metadata.getBitmap(MediaMetadata.METADATA_KEY_ART);
        }
        if (art == null) {
            art = metadata.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON);
        }
        return art;
    }

    // Drawn through a Canvas: compress cannot read a HARDWARE bitmap.
    private static byte[] crop(Bitmap art, int size) {
        int side = Math.min(art.getWidth(), art.getHeight());
        int left = (art.getWidth() - side) / 2;
        int top = (art.getHeight() - side) / 2;
        Bitmap out = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
        new Canvas(out).drawBitmap(art, new Rect(left, top, left + side, top + side), new Rect(0, 0, size, size), new Paint(Paint.FILTER_BITMAP_FLAG));
        ByteArrayOutputStream jpeg = new ByteArrayOutputStream(64 * 1024);
        boolean ok = out.compress(Bitmap.CompressFormat.JPEG, ART_QUALITY, jpeg);
        out.recycle();
        return ok ? jpeg.toByteArray() : new byte[0];
    }

    private String appName(String packageName) {
        try {
            PackageManager packages = context.getPackageManager();
            CharSequence label = packages.getApplicationLabel(packages.getApplicationInfo(packageName, 0));
            if (label != null && label.length() > 0) {
                return label.toString().trim();
            }
        } catch (Throwable t) {
            System.out.println("media app " + packageName + ": " + t);
        }
        return packageName;
    }

    private static void u32(byte[] out, int at, long value) {
        out[at] = (byte) value;
        out[at + 1] = (byte) (value >> 8);
        out[at + 2] = (byte) (value >> 16);
        out[at + 3] = (byte) (value >> 24);
    }
}
