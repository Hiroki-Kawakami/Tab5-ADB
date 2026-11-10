/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.os.Build;

import java.lang.reflect.Method;

// What the device plays, at 48 kHz stereo S16, before its volume; the device stays silent meanwhile.
final class AudioCapture implements AutoCloseable {
    static final int RATE = 48000;
    static final int CHANNELS = 2;
    private static final int REMOTE_SUBMIX = 8;
    private static final int[] MEDIA = {AudioAttributes.USAGE_MEDIA};
    private static final int[] ALL = {
        AudioAttributes.USAGE_MEDIA,
        AudioAttributes.USAGE_GAME,
        AudioAttributes.USAGE_UNKNOWN,
        AudioAttributes.USAGE_NOTIFICATION,
        AudioAttributes.USAGE_NOTIFICATION_EVENT,
        AudioAttributes.USAGE_ASSISTANCE_SONIFICATION,
        AudioAttributes.USAGE_ASSISTANCE_ACCESSIBILITY,
        AudioAttributes.USAGE_ASSISTANCE_NAVIGATION_GUIDANCE,
        AudioAttributes.USAGE_ASSISTANT,
        AudioAttributes.USAGE_ALARM,
        AudioAttributes.USAGE_NOTIFICATION_RINGTONE,
    };

    private final AudioRecord record;
    private final Object policy;

    private AudioCapture(AudioRecord record, Object policy) {
        this.record = record;
        this.policy = policy;
    }

    // `all` takes every playback but calls; otherwise media only. REMOTE_SUBMIX, the only way
    // before Android 13, takes the whole mix either way, and on Android 16 it takes nothing.
    static AudioCapture start(boolean all) throws Exception {
        if (Build.VERSION.SDK_INT < 31) {
            throw new UnsupportedOperationException("Android " + Build.VERSION.SDK_INT);
        }
        AudioCapture capture = Build.VERSION.SDK_INT >= 33 ? loopback(all ? ALL : MEDIA) : submix();
        capture.record.startRecording();
        return capture;
    }

    int read(byte[] buffer) {
        int read = 0;
        while (read < buffer.length) {
            int n = record.read(buffer, read, buffer.length - read);
            if (n <= 0) {
                return n;
            }
            read += n;
        }
        return read;
    }

    @Override
    public void close() {
        try {
            record.stop();
        } catch (RuntimeException ignored) {
        }
        record.release();
        if (policy != null) {
            unregister(policy);
        }
    }

    private static AudioFormat format() {
        return new AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT).setSampleRate(RATE).setChannelMask(AudioFormat.CHANNEL_IN_STEREO).build();
    }

    private static AudioCapture submix() throws Exception {
        AudioRecord record = new AudioRecord.Builder()
            .setContext(SystemContext.shell())
            .setAudioSource(REMOTE_SUBMIX)
            .setAudioFormat(format())
            .setBufferSizeInBytes(RATE / 10 * CHANNELS * 2)
            .build();
        if (record.getState() != AudioRecord.STATE_INITIALIZED) {
            record.release();
            throw new IllegalStateException("REMOTE_SUBMIX not initialized");
        }
        return new AudioCapture(record, null);
    }

    // AudioPlaybackCapture without MediaProjection: an AudioPolicy mix of the players with
    // these usages, looped back to a recording instead of the speaker.
    private static AudioCapture loopback(int[] usages) throws Exception {
        Context context = SystemContext.shell();
        Class<?> rule = Class.forName("android.media.audiopolicy.AudioMixingRule");
        Class<?> ruleBuilder = Class.forName("android.media.audiopolicy.AudioMixingRule$Builder");
        Object rules = ruleBuilder.getConstructor().newInstance();
        ruleBuilder.getMethod("setTargetMixRole", int.class).invoke(rules, rule.getField("MIX_ROLE_PLAYERS").getInt(null));
        int matchUsage = rule.getField("RULE_MATCH_ATTRIBUTE_USAGE").getInt(null);
        Method addRule = ruleBuilder.getMethod("addMixRule", int.class, Object.class);
        for (int usage : usages) {
            addRule.invoke(rules, matchUsage, new AudioAttributes.Builder().setUsage(usage).build());
        }
        Class<?> mix = Class.forName("android.media.audiopolicy.AudioMix");
        Class<?> mixBuilder = Class.forName("android.media.audiopolicy.AudioMix$Builder");
        Object mixes = mixBuilder.getConstructor(rule).newInstance(ruleBuilder.getMethod("build").invoke(rules));
        mixBuilder.getMethod("setFormat", AudioFormat.class).invoke(mixes, format());
        mixBuilder.getMethod("setRouteFlags", int.class).invoke(mixes, mix.getField("ROUTE_FLAG_LOOP_BACK").getInt(null));
        Object built = mixBuilder.getMethod("build").invoke(mixes);
        Class<?> policyClass = Class.forName("android.media.audiopolicy.AudioPolicy");
        Class<?> policyBuilder = Class.forName("android.media.audiopolicy.AudioPolicy$Builder");
        Object builder = policyBuilder.getConstructor(Context.class).newInstance(context);
        policyBuilder.getMethod("addMix", mix).invoke(builder, built);
        Object policy = policyBuilder.getMethod("build").invoke(builder);
        Method register = AudioManager.class.getDeclaredMethod("registerAudioPolicyStatic", policyClass);
        register.setAccessible(true);
        int result = (int) register.invoke(null, policy);
        if (result != 0) {
            throw new IllegalStateException("registerAudioPolicy " + result);
        }
        AudioRecord record = (AudioRecord) policyClass.getMethod("createAudioRecordSink", mix).invoke(policy, built);
        if (record == null || record.getState() != AudioRecord.STATE_INITIALIZED) {
            if (record != null) {
                record.release();
            }
            unregister(policy);
            throw new IllegalStateException("loopback not initialized");
        }
        return new AudioCapture(record, policy);
    }

    private static void unregister(Object policy) {
        try {
            Class<?> policyClass = Class.forName("android.media.audiopolicy.AudioPolicy");
            Method unregister;
            try {
                unregister = AudioManager.class.getDeclaredMethod("unregisterAudioPolicyStatic", policyClass);
            } catch (NoSuchMethodException e) {
                unregister = AudioManager.class.getDeclaredMethod("unregisterAudioPolicyAsyncStatic", policyClass);
            }
            unregister.setAccessible(true);
            unregister.invoke(null, policy);
        } catch (Throwable t) {
            System.out.println("audio: unregister: " + t);
        }
    }
}
