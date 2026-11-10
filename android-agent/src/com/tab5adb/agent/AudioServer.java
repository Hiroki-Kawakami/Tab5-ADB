/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import android.content.Context;
import android.media.AudioManager;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.Build;

import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

// The audio stream: one at a time, a new one ends the last.
final class AudioServer {
    private static final int HEADER_BYTES = 8;
    private static final int AUDIO_START = 0x01;
    private static final int REPLY = 0x80;
    private static final int AUDIO = 0x81;
    private static final int VOLUME = 0x82;
    private static final int OK = 0x00;
    private static final int EINVAL = 0x01;
    private static final int ENOTSUP = 0x02;
    private static final int EFAIL = 0xFF;
    private static final int CODEC_PCM = 1;
    private static final int CODEC_OPUS = 2;
    private static final int CHUNK_BYTES = AudioCapture.RATE / 100 * AudioCapture.CHANNELS * 2;
    private static final int VOLUME_CHUNKS = 10;

    private static Session current;

    private AudioServer() {
    }

    static boolean supported() {
        return Build.VERSION.SDK_INT >= 31;
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
                synchronized (AudioServer.class) {
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
        }, "audio-accept");
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
            this.thread = new Thread(this::run, "audio");
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
                    System.out.println("audio: " + e);
                }
            } catch (Exception e) {
                System.out.println("audio: " + e);
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
            long length = ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN).getInt(0) & 0xFFFFFFFFL;
            if ((head[4] & 0xFF) != AUDIO_START || length < 4 || length > 1024) {
                throw new IOException("audio stream opened with type " + (head[4] & 0xFF));
            }
            byte[] payload = new byte[(int) length];
            in.readFully(payload);
            int codec = payload[0] & 0xFF;
            boolean all = payload[1] != 0;
            if (codec != CODEC_PCM && codec != CODEC_OPUS) {
                reply(out, EINVAL, codec);
                return;
            }
            if (!supported()) {
                reply(out, ENOTSUP, codec);
                return;
            }
            OpusEncoder opus = codec == CODEC_OPUS ? OpusEncoder.create() : null;
            if (codec == CODEC_OPUS && opus == null) {
                reply(out, ENOTSUP, codec);
                return;
            }
            AudioCapture capture;
            try {
                capture = AudioCapture.start(all);
            } catch (Exception e) {
                System.out.println("audio: " + e);
                if (opus != null) {
                    opus.close();
                }
                reply(out, EFAIL, codec);
                return;
            }
            try {
                reply(out, OK, codec);
                System.out.println("audio: " + (codec == CODEC_OPUS ? "opus" : "pcm") + (all ? " all" : " media"));
                watchClose(in);
                stream(out, capture, opus);
            } finally {
                capture.close();
                if (opus != null) {
                    opus.close();
                }
            }
        }

        private void watchClose(InputStream in) {
            Thread thread = new Thread(() -> {
                try {
                    while (in.read() >= 0) {
                    }
                } catch (IOException ignored) {
                }
                stopped = true;
            }, "audio-watch");
            thread.setDaemon(true);
            thread.start();
        }

        private void stream(OutputStream out, AudioCapture capture, OpusEncoder opus) throws Exception {
            AudioManager audio = (AudioManager) SystemContext.get().getSystemService(Context.AUDIO_SERVICE);
            int max = audio.getStreamMaxVolume(AudioManager.STREAM_MUSIC);
            int volume = heard(audio);
            write(out, VOLUME, new byte[] {(byte) volume, (byte) max});
            byte[] chunk = new byte[opus != null ? OpusEncoder.FRAME_BYTES : CHUNK_BYTES];
            int chunks = 0;
            while (!stopped) {
                if (capture.read(chunk) <= 0) {
                    throw new IOException("capture ended");
                }
                if (opus == null) {
                    write(out, AUDIO, chunk);
                } else {
                    for (byte[] packet : opus.encode(chunk)) {
                        write(out, AUDIO, packet);
                    }
                }
                if (++chunks % VOLUME_CHUNKS == 0) {
                    int now = heard(audio);
                    if (now != volume) {
                        volume = now;
                        write(out, VOLUME, new byte[] {(byte) volume, (byte) max});
                    }
                }
            }
        }
    }

    private static int heard(AudioManager audio) {
        return audio.isStreamMute(AudioManager.STREAM_MUSIC) ? 0 : audio.getStreamVolume(AudioManager.STREAM_MUSIC);
    }

    private static void write(OutputStream out, int kind, byte[] payload) throws IOException {
        ByteBuffer m = ByteBuffer.allocate(HEADER_BYTES + payload.length).order(ByteOrder.LITTLE_ENDIAN);
        m.putInt(payload.length).put((byte) kind).put((byte) 0).putShort((short) 0);
        m.put(payload);
        out.write(m.array());
        out.flush();
    }

    private static void reply(OutputStream out, int status, int codec) throws IOException {
        ByteBuffer p = ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN);
        p.put((byte) status).put((byte) 0).putShort((short) 0);
        if (status == OK) {
            p.put((byte) codec).put((byte) AudioCapture.CHANNELS).putShort((short) 0).putInt(AudioCapture.RATE);
        }
        byte[] payload = new byte[status == OK ? 12 : 4];
        System.arraycopy(p.array(), 0, payload, 0, payload.length);
        write(out, REPLY, payload);
    }
}
