package com.tab5adb.agent;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

/** Raw Opus encoder for the Wi-Fi AUDIO stream (protocol.md §6). */
final class OpusEncoder implements AutoCloseable {
    static final int FRAME_MS = 20;
    static final int PCM_BYTES_PER_FRAME =
            AudioCapture.SAMPLE_RATE * FRAME_MS / 1000 * AudioCapture.CHANNELS * 2;
    private static final int BIT_RATE = 96000;
    private static final long INPUT_TIMEOUT_US = 100000;
    private static final long FIRST_OUTPUT_TIMEOUT_US = 10000;

    private final MediaCodec codec;
    private long presentationUs;

    static boolean isAvailable() {
        if (android.os.Build.VERSION.SDK_INT < 29) return false;
        try {
            return encoderName(makeFormat()) != null;
        } catch (Throwable t) {
            System.err.println("tab5adb-agent: Opus encoder query failed: " + t);
            return false;
        }
    }

    OpusEncoder() throws IOException {
        MediaFormat format = makeFormat();
        String name = encoderName(format);
        if (name == null) throw new IOException("no audio/opus encoder");
        codec = MediaCodec.createByCodecName(name);
        try {
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            codec.start();
        } catch (Throwable t) {
            codec.release();
            throw new IOException("could not start Opus encoder", t);
        }
        System.out.println("tab5adb-agent: Opus encoder started (" + name + ", "
                + BIT_RATE + "bps, " + FRAME_MS + "ms)");
    }

    private static MediaFormat makeFormat() {
        MediaFormat format = MediaFormat.createAudioFormat(
                MediaFormat.MIMETYPE_AUDIO_OPUS,
                AudioCapture.SAMPLE_RATE, AudioCapture.CHANNELS);
        format.setInteger(MediaFormat.KEY_BIT_RATE, BIT_RATE);
        format.setInteger(MediaFormat.KEY_BITRATE_MODE,
                MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR);
        format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, PCM_BYTES_PER_FRAME);
        return format;
    }

    private static String encoderName(MediaFormat format) {
        MediaCodecList codecs = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
        return codecs.findEncoderForFormat(format);
    }

    /** Feed exactly one 20 ms PCM frame and return every raw Opus packet now ready. */
    List<byte[]> encode(byte[] pcm) {
        if (pcm.length != PCM_BYTES_PER_FRAME) {
            throw new IllegalArgumentException("Opus PCM frame must be "
                    + PCM_BYTES_PER_FRAME + " bytes");
        }
        int inputIndex = codec.dequeueInputBuffer(INPUT_TIMEOUT_US);
        if (inputIndex < 0) throw new IllegalStateException("Opus input timeout");
        ByteBuffer input = codec.getInputBuffer(inputIndex);
        if (input == null || input.capacity() < pcm.length) {
            throw new IllegalStateException("short Opus input buffer");
        }
        input.clear();
        input.put(pcm);
        codec.queueInputBuffer(inputIndex, 0, pcm.length, presentationUs, 0);
        presentationUs += FRAME_MS * 1000L;
        return drainOutput();
    }

    private List<byte[]> drainOutput() {
        ArrayList<byte[]> packets = new ArrayList<>();
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        long timeoutUs = FIRST_OUTPUT_TIMEOUT_US;
        while (true) {
            int outputIndex = codec.dequeueOutputBuffer(info, timeoutUs);
            timeoutUs = 0;
            if (outputIndex == MediaCodec.INFO_TRY_AGAIN_LATER) break;
            if (outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED
                    || outputIndex == MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED) continue;
            if (outputIndex < 0) continue;
            try {
                if (info.size > 0
                        && (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
                    ByteBuffer output = codec.getOutputBuffer(outputIndex);
                    if (output == null) throw new IllegalStateException("null Opus output");
                    output.position(info.offset);
                    output.limit(info.offset + info.size);
                    byte[] packet = new byte[info.size];
                    output.get(packet);
                    packets.add(packet);
                }
            } finally {
                codec.releaseOutputBuffer(outputIndex, false);
            }
        }
        return packets;
    }

    @Override
    public void close() {
        try {
            codec.stop();
        } catch (Throwable ignore) {
        }
        codec.release();
    }
}
