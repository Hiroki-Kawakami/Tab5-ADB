/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import android.content.Context;
import android.media.AudioManager;

import java.io.IOException;
import java.io.RandomAccessFile;
import java.lang.reflect.Method;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;

// The audio capture and the media volume, which only the agent holding the
// lock on the state file may take. The file records the volume to put back.
final class AudioClaim {
    private static final String PATH = "/data/local/tmp/tab5adb-agent-audio.state";

    private static String owner;
    private static RandomAccessFile file;
    private static FileLock lock;
    private static int wasVolume;
    private static boolean wasMuted;
    private static boolean changed;

    private AudioClaim() {
    }

    // Puts back what a dead agent left, unless a live one holds the claim.
    static synchronized void recover(String id) {
        owner = id;
        try {
            if (lock()) {
                unlock();
            }
        } catch (IOException e) {
            System.out.println("audio claim: " + e);
        }
    }

    // false while another agent holds the claim; otherwise records the volume to put back.
    static synchronized boolean claim() throws Exception {
        if (lock != null) {
            return true;
        }
        if (!lock()) {
            return false;
        }
        AudioManager audio = audio();
        wasVolume = audible(audio);
        wasMuted = audio.isStreamMute(AudioManager.STREAM_MUSIC);
        String state = owner + " " + wasVolume + " " + (wasMuted ? 1 : 0) + "\n";
        file.seek(0);
        file.write(state.getBytes(StandardCharsets.UTF_8));
        file.getFD().sync();
        changed = true;
        return true;
    }

    // Puts the recorded volume back and keeps the claim, for while the capture still silences the device.
    static synchronized void putBack() {
        if (lock == null || !changed) {
            return;
        }
        changed = false;
        set(wasVolume, wasMuted);
    }

    static synchronized void release() {
        if (lock == null) {
            return;
        }
        putBack();
        try {
            unlock();
        } catch (IOException e) {
            System.out.println("audio claim: " + e);
        }
    }

    static AudioManager audio() throws Exception {
        return (AudioManager) SystemContext.get().getSystemService(Context.AUDIO_SERVICE);
    }

    // The step heard once unmuted; getStreamVolume() answers 0 while muted.
    static int audible(AudioManager audio) {
        try {
            Method last = AudioManager.class.getMethod("getLastAudibleStreamVolume", int.class);
            return (int) last.invoke(audio, AudioManager.STREAM_MUSIC);
        } catch (Throwable t) {
            return audio.getStreamVolume(AudioManager.STREAM_MUSIC);
        }
    }

    static void set(int volume, boolean muted) {
        try {
            AudioManager audio = audio();
            if (audio.isStreamMute(AudioManager.STREAM_MUSIC)) {
                audio.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_UNMUTE, 0);
            }
            audio.setStreamVolume(AudioManager.STREAM_MUSIC, Math.min(volume, audio.getStreamMaxVolume(AudioManager.STREAM_MUSIC)), 0);
            if (muted) {
                audio.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_MUTE, 0);
            }
        } catch (Throwable t) {
            System.out.println("audio claim: volume: " + t);
        }
    }

    private static boolean lock() throws IOException {
        RandomAccessFile f = new RandomAccessFile(PATH, "rw");
        FileLock l;
        try {
            l = f.getChannel().tryLock();
        } catch (IOException e) {
            f.close();
            throw e;
        }
        if (l == null) {
            f.close();
            return false;
        }
        file = f;
        lock = l;
        byte[] left = new byte[(int) f.length()];
        f.readFully(left);
        if (left.length > 0) {
            restore(new String(left, StandardCharsets.UTF_8));
            f.setLength(0);
        }
        return true;
    }

    private static void unlock() throws IOException {
        file.setLength(0);
        lock.release();
        file.close();
        lock = null;
        file = null;
    }

    private static void restore(String state) {
        String[] f = state.trim().split(" ");
        if (f.length < 3 || !f[1].matches("\\d+") || !f[2].matches("[01]")) {
            System.out.println("audio claim: bad state " + state.trim());
            return;
        }
        System.out.println("audio: restoring what " + f[0] + " left");
        set(Integer.parseInt(f[1]), f[2].equals("1"));
    }
}
