package com.tab5adb.agent;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;

import java.lang.reflect.Method;

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
 * <p><b>Native sample rate</b>: REMOTE_SUBMIX builds an audio patch from the
 * device's primary output mix to our capture buffer, and that patch only carries
 * data when the source and sink sample rates <i>match</i>. Newer devices output at
 * high rates (e.g. Pixel 9 Pro / Pixel 10 on Android 16 run the primary mix at
 * 192 kHz), so a fixed 48 kHz capture creates a rate-mismatched patch and
 * {@code AudioRecord.read} just returns 0 forever (silent — the bug seen on the
 * Pixel 10). So we capture at the device's <b>native output rate</b>
 * ({@link #nativeOutputRate}) and resample to the wire's 48 kHz here, keeping the
 * §6.2 wire format fixed at PCM_S16LE / 48 kHz / stereo. When the native rate is
 * already 48 kHz the resampler is bypassed (zero-copy fast path).
 *
 * <p><b>Test path</b> ({@code testTone=true}, paired with {@code Server --test-tone}):
 * a deterministic sine generated in real time, so the headless agent_link test
 * exercises the AUDIO framing without the capture HW (the audio analogue of
 * {@code --test-pattern}).
 *
 * <p>Both {@code AudioRecord} and the generator produce native-endian (=
 * little-endian on ARM) 16-bit samples, which is already the wire format (§6.2), so
 * {@link #read} bytes go straight onto the wire with no endianness conversion.
 */
final class AudioCapture {
    static final int SAMPLE_RATE = 48000;     // WIRE/output rate (§6.2), after resampling
    static final int CHANNELS = 2;            // stereo (the Tab5 BSP downmixes for speaker)
    static final int CODEC_PCM_S16LE = 0x01;  // protocol.md §6 audio_codec
    // ~10 ms stereo chunk at the OUTPUT rate: 480 frames * 2 ch * 2 bytes. Small for
    // low latency and so it interleaves with the JPEG flow without bursts (§6.3).
    static final int CHUNK_BYTES = (SAMPLE_RATE / 100) * CHANNELS * 2;  // 1920

    private static final int REMOTE_SUBMIX = 8;  // MediaRecorder.AudioSource (hidden)

    private final boolean testTone;
    private volatile AudioRecord record;
    private int captureRate;     // the AudioRecord rate (device native output rate)
    private boolean resampling;  // captureRate != SAMPLE_RATE
    private Object audioPolicy;  // registered AudioPolicy for the loopback path (unregister on close)

    // Linear-resampler state (interleaved L,R), carried across read() calls so the
    // stream stays continuous (no per-chunk discontinuity / click).
    private short[] rs;          // input frames not yet fully consumed (interleaved)
    private int rsFrames;        // valid frames in `rs`
    private double rsPos;        // fractional read position (frames) relative to rs[0]

    // Device-volume tracking. The loopback mix (ROUTE_FLAG_LOOP_BACK) captures the
    // media stream BEFORE the output device volume is applied (full fixed scale), so
    // unlike REMOTE_SUBMIX (which captured post-volume) the phone's volume slider
    // would NOT affect the delivered audio. To preserve the expected behaviour —
    // changing the phone's media volume changes the streamed loudness — we apply the
    // current STREAM_MUSIC volume as a software gain here. REMOTE_SUBMIX needs none.
    private boolean applyVolume;
    private AudioManager audioManager;
    private volatile float volGain = 1f;
    private long lastVolNs;

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
        // Preferred on Android 13+ (API 33): the AudioPolicy loopback path captures
        // USAGE_MEDIA players directly. REMOTE_SUBMIX only captures audio the system
        // *reroutes* to the submix, and on Android 16 that rerouting no longer
        // happens for normal media — capture initialises but is (near-)silent (the
        // Pixel 10 symptom: media keeps playing on the speaker, the submix gets
        // nothing). The loopback mix doesn't depend on output routing.
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            AudioRecord pb = createPlaybackRecord();
            if (pb != null) {
                captureRate = SAMPLE_RATE;
                resampling = false;
                // The loopback mix is pre-device-volume; apply STREAM_MUSIC volume in
                // software so the phone's volume (incl. the mirror's Vol± keys) is
                // reflected in the stream, as the old REMOTE_SUBMIX capture was.
                try {
                    audioManager = (AudioManager) FakeContext.get()
                            .getSystemService(android.content.Context.AUDIO_SERVICE);
                    applyVolume = audioManager != null;
                } catch (Throwable t) {
                    applyVolume = false;
                }
                pb.startRecording();
                record = pb;
                System.out.println("tab5adb-agent: audio capture started (playback-capture "
                        + "loopback, " + SAMPLE_RATE + "Hz stereo, volume="
                        + (applyVolume ? "tracked" : "fixed") + ")");
                return;
            }
            System.err.println("tab5adb-agent: audio: playback-capture unavailable, "
                    + "falling back to REMOTE_SUBMIX");
        }
        int nativeRate = nativeOutputRate();
        AudioRecord r = buildRecord(nativeRate);
        if (r == null && nativeRate != SAMPLE_RATE) {
            // Native rate refused (some devices won't open REMOTE_SUBMIX at the
            // primary-output rate); fall back to 48 kHz (the historic path).
            System.err.println("tab5adb-agent: audio: capture at " + nativeRate
                    + "Hz failed, retrying at " + SAMPLE_RATE + "Hz");
            nativeRate = SAMPLE_RATE;
            r = buildRecord(nativeRate);
        }
        if (r == null) {
            throw new IllegalStateException("AudioRecord not INITIALIZED (REMOTE_SUBMIX)");
        }
        captureRate = nativeRate;
        resampling = captureRate != SAMPLE_RATE;
        r.startRecording();
        record = r;
        System.out.println("tab5adb-agent: audio capture started (REMOTE_SUBMIX, capture="
                + captureRate + "Hz -> wire " + SAMPLE_RATE + "Hz stereo"
                + (resampling ? ", resampling)" : ")"));
    }

    /** Build + validate an AudioRecord at {@code rate}; null if not INITIALIZED. */
    private AudioRecord buildRecord(int rate) {
        int minBuf = AudioRecord.getMinBufferSize(rate,
                AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        if (minBuf <= 0) return null;
        int bufBytes = Math.max(minBuf, rate * CHANNELS * 2 / 5);  // ~200 ms
        AudioRecord r;
        try {
            AudioRecord.Builder b = new AudioRecord.Builder();
            // Android 12+: AudioRecord attributes its CAPTURE_AUDIO_OUTPUT appop to
            // the caller's AttributionSource. app_process has none, so without a
            // shell-package context the recorder is STATE_UNINITIALIZED on newer
            // devices (REMOTE_SUBMIX silently fails on a Pixel 10 / Android 16).
            // The FakeContext (scrcpy workaround) makes the appop resolve to shell.
            if (android.os.Build.VERSION.SDK_INT >= 31) {
                try {
                    b.setContext(FakeContext.get());
                } catch (Throwable t) {
                    System.err.println("tab5adb-agent: audio: FakeContext unavailable: " + t);
                }
            }
            r = b.setAudioSource(REMOTE_SUBMIX)
                    .setAudioFormat(new AudioFormat.Builder()
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .setSampleRate(rate)
                            .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                            .build())
                    .setBufferSizeInBytes(bufBytes)
                    .build();
        } catch (Throwable t) {
            return null;
        }
        if (r.getState() != AudioRecord.STATE_INITIALIZED) {
            r.release();
            return null;
        }
        return r;
    }

    /**
     * Build a capture {@link AudioRecord} via an AudioPolicy loopback mix that
     * matches {@code USAGE_MEDIA} players (the scrcpy "playback" source, ported with
     * reflection since {@code android.media.audiopolicy.*} is hidden). Uses
     * {@code ROUTE_FLAG_LOOP_BACK} (capture only — the device output is muted, =
     * "Tab5 only"). Returns null (and logs) if the policy can't be registered.
     */
    private AudioRecord createPlaybackRecord() {
        try {
            Class<?> ruleClass = Class.forName("android.media.audiopolicy.AudioMixingRule");
            Class<?> ruleBuilderClass =
                    Class.forName("android.media.audiopolicy.AudioMixingRule$Builder");
            Object ruleBuilder = ruleBuilderClass.getConstructor().newInstance();
            int mixRolePlayers = ruleClass.getField("MIX_ROLE_PLAYERS").getInt(null);
            ruleBuilderClass.getMethod("setTargetMixRole", int.class)
                    .invoke(ruleBuilder, mixRolePlayers);
            AudioAttributes attrs =
                    new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build();
            int ruleMatchUsage = ruleClass.getField("RULE_MATCH_ATTRIBUTE_USAGE").getInt(null);
            ruleBuilderClass.getMethod("addMixRule", int.class, Object.class)
                    .invoke(ruleBuilder, ruleMatchUsage, attrs);
            Object rule = ruleBuilderClass.getMethod("build").invoke(ruleBuilder);

            Class<?> mixClass = Class.forName("android.media.audiopolicy.AudioMix");
            Class<?> mixBuilderClass = Class.forName("android.media.audiopolicy.AudioMix$Builder");
            Object mixBuilder = mixBuilderClass.getConstructor(ruleClass).newInstance(rule);
            AudioFormat fmt = new AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(SAMPLE_RATE)
                    .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                    .build();
            mixBuilderClass.getMethod("setFormat", AudioFormat.class).invoke(mixBuilder, fmt);
            int routeLoopBack = mixClass.getField("ROUTE_FLAG_LOOP_BACK").getInt(null);
            mixBuilderClass.getMethod("setRouteFlags", int.class).invoke(mixBuilder, routeLoopBack);
            Object mix = mixBuilderClass.getMethod("build").invoke(mixBuilder);

            Class<?> policyClass = Class.forName("android.media.audiopolicy.AudioPolicy");
            Class<?> policyBuilderClass =
                    Class.forName("android.media.audiopolicy.AudioPolicy$Builder");
            Object policyBuilder = policyBuilderClass
                    .getConstructor(android.content.Context.class).newInstance(FakeContext.get());
            policyBuilderClass.getMethod("addMix", mixClass).invoke(policyBuilder, mix);
            Object policy = policyBuilderClass.getMethod("build").invoke(policyBuilder);

            Method reg = AudioManager.class.getDeclaredMethod(
                    "registerAudioPolicyStatic", policyClass);
            reg.setAccessible(true);
            int result = (int) reg.invoke(null, policy);
            if (result != 0) {
                throw new IllegalStateException("registerAudioPolicy returned " + result);
            }
            audioPolicy = policy;
            AudioRecord rec = (AudioRecord) policyClass
                    .getMethod("createAudioRecordSink", mixClass).invoke(policy, mix);
            if (rec == null || rec.getState() != AudioRecord.STATE_INITIALIZED) {
                System.err.println("tab5adb-agent: audio: playback-capture sink not initialized");
                if (rec != null) rec.release();
                unregisterPolicy();
                return null;
            }
            return rec;
        } catch (Throwable t) {
            System.err.println("tab5adb-agent: audio: playback-capture setup failed: " + t);
            unregisterPolicy();
            return null;
        }
    }

    /** Best-effort unregister of the loopback {@link #audioPolicy} (idempotent). */
    private void unregisterPolicy() {
        Object policy = audioPolicy;
        audioPolicy = null;
        if (policy == null) return;
        try {
            Class<?> policyClass = Class.forName("android.media.audiopolicy.AudioPolicy");
            Method unreg;
            try {
                unreg = AudioManager.class.getDeclaredMethod(
                        "unregisterAudioPolicyStatic", policyClass);
            } catch (NoSuchMethodException e) {
                unreg = AudioManager.class.getDeclaredMethod(
                        "unregisterAudioPolicyAsyncStatic", policyClass);
            }
            unreg.setAccessible(true);
            unreg.invoke(null, policy);
        } catch (Throwable ignore) {
            // leak the policy rather than crash; the process is short-lived
        }
    }

    /**
     * The device's primary-output sample rate (the rate the REMOTE_SUBMIX patch
     * runs at). Reads {@code AudioSystem.getPrimaryOutputSamplingRate()} (hidden,
     * reachable from shell uid via reflection); falls back to 48 kHz.
     */
    private static int nativeOutputRate() {
        try {
            Class<?> as = Class.forName("android.media.AudioSystem");
            Method m = as.getMethod("getPrimaryOutputSamplingRate");
            Object v = m.invoke(null);
            if (v instanceof Integer) {
                int rate = (Integer) v;
                if (rate >= 8000 && rate <= 768000) return rate;
            }
        } catch (Throwable ignore) {
            // hidden API unavailable on this device/version — use the default
        }
        return SAMPLE_RATE;
    }

    /**
     * Block for the next PCM chunk into {@code buf} (length = {@link #CHUNK_BYTES},
     * at the 48 kHz output rate); returns the number of bytes read, or &lt; 0 on
     * error / after {@link #close}. Real-time paced in test mode.
     */
    int read(byte[] buf) {
        if (testTone) return genTone(buf);
        AudioRecord r = record;
        if (r == null) return -1;
        int n = resampling ? resampleRead(r, buf) : r.read(buf, 0, buf.length);
        if (n > 0 && applyVolume) applyDeviceVolume(buf, n);
        return n;
    }

    /** Scale {@code n} bytes of interleaved s16-LE in {@code buf} by the current
     *  STREAM_MUSIC volume (so the phone's volume governs the streamed loudness). */
    private void applyDeviceVolume(byte[] buf, int n) {
        float g = currentVolumeGain();
        if (g >= 0.999f) return;  // unity — leave the samples untouched
        if (g <= 0f) {            // muted
            java.util.Arrays.fill(buf, 0, n, (byte) 0);
            return;
        }
        for (int i = 0; i + 1 < n; i += 2) {
            int s = (short) ((buf[i] & 0xff) | (buf[i + 1] << 8));
            int v = Math.round(s * g);
            if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
            buf[i] = (byte) v;
            buf[i + 1] = (byte) (v >> 8);
        }
    }

    /** Linear gain (0..1) for the current STREAM_MUSIC volume, via the system's
     *  index→dB curve; cached and refreshed ~every 100 ms (volume changes live). */
    private float currentVolumeGain() {
        long now = System.nanoTime();
        if (now - lastVolNs >= 100_000_000L) {
            lastVolNs = now;
            try {
                int idx = audioManager.getStreamVolume(AudioManager.STREAM_MUSIC);
                if (idx <= 0) {
                    volGain = 0f;
                } else {
                    float db = audioManager.getStreamVolumeDb(AudioManager.STREAM_MUSIC,
                            idx, android.media.AudioDeviceInfo.TYPE_BUILTIN_SPEAKER);
                    float g = (float) Math.pow(10.0, db / 20.0);
                    volGain = g > 1f ? 1f : (g < 0f ? 0f : g);
                }
            } catch (Throwable ignore) {
                // keep the last gain
            }
        }
        return volGain;
    }

    /**
     * Read native-rate frames from {@code r} and linear-resample them into {@code out}
     * (48 kHz, interleaved s16 LE). Returns bytes written, or &lt; 0 on error/close.
     */
    private int resampleRead(AudioRecord r, byte[] out) {
        final int outFrames = out.length / (CHANNELS * 2);
        final double ratio = (double) captureRate / SAMPLE_RATE;
        // Frames of input needed in `rs`: enough to interpolate output 0..outFrames-1
        // AND to advance the read cursor by outFrames*ratio (so nothing is dropped).
        final int needed = (int) Math.floor(rsPos + outFrames * ratio) + 2;
        if (rs == null || rs.length < needed * 2) {
            short[] grown = new short[needed * 2];
            if (rsFrames > 0) System.arraycopy(rs, 0, grown, 0, rsFrames * 2);
            rs = grown;
        }
        while (rsFrames < needed) {
            int wantShorts = (needed - rsFrames) * 2;
            int got = r.read(rs, rsFrames * 2, wantShorts);  // shorts read
            if (got <= 0) return -1;  // error or stopped (READ_BLOCKING won't return 0)
            rsFrames += got / 2;      // full stereo frames (drop a trailing odd short)
        }

        int o = 0;
        for (int j = 0; j < outFrames; j++) {
            double p = rsPos + j * ratio;
            int i = (int) p;
            double f = p - i;
            int b = i * 2;
            short l0 = rs[b],     r0 = rs[b + 1];
            short l1 = rs[b + 2], r1 = rs[b + 3];
            short l = (short) Math.round(l0 + (l1 - l0) * f);
            short rr = (short) Math.round(r0 + (r1 - r0) * f);
            out[o++] = (byte) l;  out[o++] = (byte) (l >> 8);   // L, LE
            out[o++] = (byte) rr; out[o++] = (byte) (rr >> 8);  // R, LE
        }

        // Advance: total source consumed = outFrames*ratio. Drop the integer part
        // from the front of `rs`, keep the fractional remainder + read-ahead frames
        // so the next chunk continues the same phase (no boundary click).
        double endPos = rsPos + outFrames * ratio;
        int adv = (int) endPos;
        int keep = rsFrames - adv;
        if (keep > 0) System.arraycopy(rs, adv * 2, rs, 0, keep * 2);
        rsFrames = Math.max(keep, 0);
        rsPos = endPos - adv;
        return o;
    }

    /** Stop + release. Idempotent (also unblocks a blocking {@link #read}). */
    void close() {
        AudioRecord r = record;
        record = null;
        if (r != null) {
            try { r.stop(); } catch (Throwable ignore) {}
            r.release();
        }
        unregisterPolicy();  // no-op unless the loopback path was used
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
