/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Build;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

// 20 ms of 48 kHz stereo S16 in, raw Opus packets or AAC-LC access units out.
final class AudioEncoder implements AutoCloseable {
    static final int FRAME_BYTES = AudioCapture.RATE / 50 * AudioCapture.CHANNELS * 2;
    private static final int OPUS_BIT_RATE = 96000;
    private static final int AAC_BIT_RATE = 128000;
    private static final long INPUT_TIMEOUT_US = 100_000;
    private static final long OUTPUT_TIMEOUT_US = 10_000;

    private final MediaCodec codec;
    private final String name;
    private long timeUs;

    private AudioEncoder(MediaCodec codec, String name) {
        this.codec = codec;
        this.name = name;
    }

    // Null when the device has no Opus encoder.
    static AudioEncoder opus() throws IOException {
        if (Build.VERSION.SDK_INT < 29) {
            return null;
        }
        MediaFormat format = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, AudioCapture.RATE, AudioCapture.CHANNELS);
        format.setInteger(MediaFormat.KEY_BIT_RATE, OPUS_BIT_RATE);
        format.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR);
        return create("opus", format);
    }

    // Null when the device has no AAC encoder.
    static AudioEncoder aac() throws IOException {
        MediaFormat format = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, AudioCapture.RATE, AudioCapture.CHANNELS);
        format.setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC);
        format.setInteger(MediaFormat.KEY_BIT_RATE, AAC_BIT_RATE);
        return create("aac", format);
    }

    private static AudioEncoder create(String what, MediaFormat format) throws IOException {
        format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, FRAME_BYTES);
        String name = new MediaCodecList(MediaCodecList.REGULAR_CODECS).findEncoderForFormat(format);
        if (name == null) {
            return null;
        }
        MediaCodec codec = MediaCodec.createByCodecName(name);
        try {
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            codec.start();
        } catch (RuntimeException e) {
            codec.release();
            throw new IOException(what + " " + name, e);
        }
        return new AudioEncoder(codec, what);
    }

    List<byte[]> encode(byte[] pcm) {
        int input = codec.dequeueInputBuffer(INPUT_TIMEOUT_US);
        if (input < 0) {
            throw new IllegalStateException(name + " input timed out");
        }
        ByteBuffer buffer = codec.getInputBuffer(input);
        buffer.clear();
        buffer.put(pcm);
        codec.queueInputBuffer(input, 0, pcm.length, timeUs, 0);
        timeUs += 20_000;
        List<byte[]> packets = new ArrayList<>();
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        long timeout = OUTPUT_TIMEOUT_US;
        while (true) {
            int output = codec.dequeueOutputBuffer(info, timeout);
            timeout = 0;
            if (output == MediaCodec.INFO_TRY_AGAIN_LATER) {
                return packets;
            }
            if (output < 0) {
                continue;
            }
            if (info.size > 0 && (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
                ByteBuffer out = codec.getOutputBuffer(output);
                byte[] packet = new byte[info.size];
                out.position(info.offset);
                out.get(packet);
                packets.add(packet);
            }
            codec.releaseOutputBuffer(output, false);
        }
    }

    @Override
    public void close() {
        try {
            codec.stop();
        } catch (RuntimeException ignored) {
        }
        codec.release();
    }
}
