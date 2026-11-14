/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import java.util.ArrayDeque;

// The bytes the video stream has written and the Tab5 has acknowledged, and
// from them what the link carries and when the next frame may follow.
final class VideoPacer {
    private static final long RATE_WINDOW_NS = 1_000_000_000L;
    private static final long LATENCY_WINDOW_NS = 2_000_000_000L;
    private static final long MIN_BUSY_NS = 20_000_000L;

    private static final class Strip {
        final long end;
        final long wrote;
        final long ahead;
        final int bytes;

        Strip(long end, long wrote, long ahead, int bytes) {
            this.end = end;
            this.wrote = wrote;
            this.ahead = ahead;
            this.bytes = bytes;
        }
    }

    private static final class Mark {
        final long at;
        final long acked;
        final long busy;

        Mark(long at, long acked, long busy) {
            this.at = at;
            this.acked = acked;
            this.busy = busy;
        }
    }

    private final ArrayDeque<Strip> strips = new ArrayDeque<>();
    private final ArrayDeque<Mark> marks = new ArrayDeque<>();
    // Increasing latencies, the oldest first, so the first is the window's least.
    private final ArrayDeque<long[]> latencies = new ArrayDeque<>();
    private long written;
    private long acked;
    private long busy;
    private long lastEvent;
    private double bytesPerNs;

    void wrote(int bytes, long now) {
        advance(now);
        long ahead = written - acked;
        written += bytes;
        strips.addLast(new Strip(written, now, ahead, bytes));
    }

    // ack: the low 32 bits of the bytes the Tab5 has read.
    void acked(long ack, long now) {
        advance(now);
        acked = Math.min(written, acked + ((ack - acked) & 0xFFFFFFFFL));
        marks.addLast(new Mark(now, acked, busy));
        while (marks.size() > 1) {
            Mark first = marks.pollFirst();
            if (marks.peekFirst().at > now - RATE_WINDOW_NS) {
                marks.addFirst(first);
                break;
            }
        }
        Mark base = marks.peekFirst();
        if (busy - base.busy >= MIN_BUSY_NS) {
            bytesPerNs = (double) (acked - base.acked) / (busy - base.busy);
        }
        while (!strips.isEmpty() && strips.peekFirst().end <= acked) {
            Strip s = strips.pollFirst();
            if (bytesPerNs > 0) {
                long latency = Math.max(0, now - s.wrote - (long) ((s.ahead + s.bytes) / bytesPerNs));
                while (!latencies.isEmpty() && latencies.peekLast()[1] >= latency) {
                    latencies.pollLast();
                }
                latencies.addLast(new long[]{now, latency});
            }
        }
        while (!latencies.isEmpty() && latencies.peekFirst()[0] < now - LATENCY_WINDOW_NS) {
            latencies.pollFirst();
        }
    }

    private void advance(long now) {
        if (written > acked) {
            busy += now - lastEvent;
        }
        lastEvent = now;
    }

    // Once what is still in flight takes no longer to arrive than an ack
    // takes to come back, so the link does not idle between frames.
    boolean mayStart() {
        long inFlight = written - acked;
        return inFlight == 0 || (!latencies.isEmpty() && inFlight <= bytesPerNs * latencies.peekFirst()[1]);
    }

    // 0 until measured.
    double bytesPerSecond() {
        return bytesPerNs * 1e9;
    }

    // -1 until measured.
    long latencyNs() {
        return latencies.isEmpty() ? -1 : latencies.peekFirst()[1];
    }
}
