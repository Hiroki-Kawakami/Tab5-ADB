package com.tab5adb.agent;

import android.media.AudioFormat;
import android.media.AudioRecord;

/**
 * Device audio capture for the AUDIO stream (protocol.md §6). Targets Android 12+.
 *
 * <p><b>Real path</b>: {@link AudioRecord} on the hidden {@code REMOTE_SUBMIX}
 * source (the scrcpy technique — app_process runs with shell uid, which holds
 * {@code CAPTURE_AUDIO_OUTPUT}, so no MediaProjection and no permission dialog).
 * Capturing REMOTE_SUBMIX <i>reroutes</i> the device's output mix, so the phone
 * speaker goes silent while mirroring — exactly the "Tab5 only" mode (verified on
 * Android 14). PhoneOnly never constructs this (no AUDIO stream).
 *
 * <p><b>Test path</b> ({@code testTone=true}, paired with {@code Server --test-tone}):
 * a deterministic sine generated in real time, so the headless agent_link test
 * exercises the AUDIO framing without the capture HW (the audio analogue of
 * {@code --test-pattern}).
 *
 * <p>v1 streams raw PCM_S16LE / 48 kHz / stereo. Both {@code AudioRecord} and the
 * generator produce native-endian (= little-endian on ARM) 16-bit samples, which
 * is already the wire format (§6.2), so {@link #read} bytes go straight onto the
 * wire with no conversion.
 */
final class AudioCapture {
    static final int SAMPLE_RATE = 48000;
    static final int CHANNELS = 2;            // stereo (the Tab5 BSP downmixes for speaker)
    static final int CODEC_PCM_S16LE = 0x01;  // protocol.md §6 audio_codec
    // ~10 ms stereo chunk: 480 frames * 2 ch * 2 bytes. Small for low latency and so
    // it interleaves with the JPEG flow without bursts (protocol.md §6.3).
    static final int CHUNK_BYTES = (SAMPLE_RATE / 100) * CHANNELS * 2;  // 1920

    private static final int REMOTE_SUBMIX = 8;  // MediaRecorder.AudioSource (hidden)

    private final boolean testTone;
    private volatile AudioRecord record;

    // Test-tone state (real-time pacing).
    private double phase;
    private long toneStartNs;
    private long toneSamples;

    AudioCapture(boolean testTone) {
        this.testTone = testTone;
    }

    /** Start capturing; throws if the real AudioRecord can't be created/started. */
    void start() {
        if (testTone) {
            toneStartNs = System.nanoTime();
            return;
        }
        int minBuf = AudioRecord.getMinBufferSize(SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        int bufBytes = Math.max(minBuf, SAMPLE_RATE * CHANNELS * 2 / 5);  // ~200 ms
        AudioRecord r = new AudioRecord.Builder()
                .setAudioSource(REMOTE_SUBMIX)
                .setAudioFormat(new AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(SAMPLE_RATE)
                        .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                        .build())
                .setBufferSizeInBytes(bufBytes)
                .build();
        if (r.getState() != AudioRecord.STATE_INITIALIZED) {
            r.release();
            throw new IllegalStateException("AudioRecord not INITIALIZED (REMOTE_SUBMIX)");
        }
        r.startRecording();
        record = r;
        System.out.println("tab5adb-agent: audio capture started (REMOTE_SUBMIX, "
                + SAMPLE_RATE + "Hz stereo)");
    }

    /**
     * Block for the next PCM chunk into {@code buf} (length = {@link #CHUNK_BYTES});
     * returns the number of bytes read, or &lt; 0 on error / after {@link #close}.
     * Real-time paced in test mode.
     */
    int read(byte[] buf) {
        if (testTone) return genTone(buf);
        AudioRecord r = record;
        if (r == null) return -1;
        return r.read(buf, 0, buf.length);  // READ_BLOCKING (default)
    }

    /** Stop + release. Idempotent (also unblocks a blocking {@link #read}). */
    void close() {
        AudioRecord r = record;
        record = null;
        if (r != null) {
            try { r.stop(); } catch (Throwable ignore) {}
            r.release();
        }
    }

    // --- test tone: 440 Hz stereo sine, real-time paced ---
    private int genTone(byte[] buf) {
        int frames = buf.length / (CHANNELS * 2);
        double step = 2 * Math.PI * 440.0 / SAMPLE_RATE;
        int o = 0;
        for (int i = 0; i < frames; i++) {
            short s = (short) (Math.sin(phase) * 8000);
            phase += step;
            if (phase > 2 * Math.PI) phase -= 2 * Math.PI;
            buf[o++] = (byte) s; buf[o++] = (byte) (s >> 8);  // L, LE
            buf[o++] = (byte) s; buf[o++] = (byte) (s >> 8);  // R, LE
        }
        // Pace to real time: this chunk advances the stream by `frames` samples.
        toneSamples += frames;
        long targetNs = toneStartNs + toneSamples * 1_000_000_000L / SAMPLE_RATE;
        long sleepNs = targetNs - System.nanoTime();
        if (sleepNs > 0) {
            try {
                Thread.sleep(sleepNs / 1_000_000L, (int) (sleepNs % 1_000_000L));
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return -1;
            }
        }
        return o;
    }
}
