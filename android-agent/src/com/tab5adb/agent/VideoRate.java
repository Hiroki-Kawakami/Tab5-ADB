/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import java.util.ArrayDeque;

// JPEG quality and frame rate: kept under maxRate bytes/s and, when adapting,
// under what the link carries, lowering quality before frame rate.
final class VideoRate {
    private static final long WINDOW_NS = 1_000_000_000L;
    private static final long RAISE_INTERVAL_NS = 100_000_000L;
    private static final int MIN_QUALITY = 20;
    private static final int ADAPT_MIN_QUALITY = 40;
    private static final int ADAPT_MIN_FPS = 10;

    private final int ceiling;
    private final int maxFps;
    private final long maxRate;
    private final boolean adapt;
    private final ArrayDeque<Long> sent = new ArrayDeque<>();
    private final ArrayDeque<Long> encoded = new ArrayDeque<>();
    private int quality;
    private int fps;
    private int lastBytes;
    private long raised = -1;
    private long fpsChanged = -1;

    VideoRate(int ceiling, int maxFps, long maxRate, boolean adapt) {
        this.ceiling = ceiling;
        this.maxFps = maxFps;
        this.maxRate = maxRate;
        this.adapt = adapt;
        quality = ceiling;
        fps = maxFps;
    }

    int quality() {
        return quality;
    }

    // 0 for no limit.
    int fps() {
        return fps;
    }

    void encoded(long now) {
        encoded.addLast(now);
    }

    void sent(int bytes, long now) {
        sent.addLast(now);
        lastBytes = bytes;
    }

    // linkRate: bytes/s, 0 while unknown.
    void update(long now, double linkRate) {
        if (maxRate == 0 && !adapt) {
            return;
        }
        trim(sent, now);
        trim(encoded, now);
        double need = 0;
        if (maxRate > 0) {
            need = (double) lastBytes * sent.size() / maxRate;
        }
        if (adapt && linkRate > 0) {
            need = Math.max(need, lastBytes * encoded.size() / linkRate);
        }
        int floor = Math.min(adapt ? ADAPT_MIN_QUALITY : MIN_QUALITY, ceiling);
        if (raised < 0) {
            raised = now;
            fpsChanged = now;
        }
        // The frame rate waits a window between changes, for the counts to
        // show the last one.
        boolean fpsDue = now - fpsChanged >= WINDOW_NS;
        if (need > 1) {
            if (quality > floor) {
                quality = Math.max(floor, quality - Math.max(1, (int) Math.ceil((need - 1) * 20)));
            } else if (adapt && fps > ADAPT_MIN_FPS && fpsDue) {
                fps = Math.max(ADAPT_MIN_FPS, (int) (fps / need));
                fpsChanged = now;
            }
        } else if (need < 0.8 && now - raised >= RAISE_INTERVAL_NS) {
            int step = need < 0.5 ? 5 : 1;
            if (adapt && fps < maxFps) {
                if (fpsDue) {
                    fps = Math.min(maxFps, fps + step);
                    fpsChanged = now;
                }
            } else {
                quality = Math.min(ceiling, quality + step);
            }
            raised = now;
        }
    }

    private static void trim(ArrayDeque<Long> times, long now) {
        while (!times.isEmpty() && now - times.peekFirst() >= WINDOW_NS) {
            times.pollFirst();
        }
    }
}
