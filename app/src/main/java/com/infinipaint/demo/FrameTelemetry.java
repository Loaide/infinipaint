package com.infinipaint.demo;

import java.util.function.LongSupplier;
import java.util.Arrays;

/** Per-Activity counters. Only the explicit dump allocates; no lock spans rendering or I/O. */
final class FrameTelemetry {
    enum Reason { CAMERA, STYLUS, OTHER }

    private final LongSupplier clock;
    private long camera, stylus, other, callbacks, matched, firstNs, waitSumNs, waitMaxNs;
    private long renders, nativeSumNs, nativeMaxNs;
    private boolean pending;
    private Snapshot mark;
    private final Window waits = new Window(), nativeTimes = new Window(), callbackTimes = new Window();
    // Most recent 512 observations since mark. O(1) writes; quantiles only on dump.
    private static final class Window {
        final long[] values = new long[512];
        int next, size;
        void add(long ns) {
            values[next] = ns;
            next = (next + 1) % values.length;
            size = Math.min(size + 1, values.length);
        }
        void reset() { next = size = 0; }
        long[] summary() {
            if (size == 0) return new long[5];
            long[] ordered = Arrays.copyOf(values, size);
            Arrays.sort(ordered);
            return new long[]{size, ordered[(size*50-1)/100], ordered[(size*95-1)/100],
                    ordered[(size*99-1)/100], ordered[size-1]};
        }
    }

    FrameTelemetry(LongSupplier clock) { this.clock = clock; }

    synchronized void request(Reason reason) {
        if (reason == Reason.CAMERA) camera++;
        else if (reason == Reason.STYLUS) stylus++;
        else other++;
        if (!pending) {
            firstNs = clock.getAsLong();
            pending = true;
        }
    }

    synchronized void drawStarted() {
        callbacks++;
        if (pending) {
            long wait = clock.getAsLong() - firstNs;
            waitSumNs += wait;
            waits.add(wait);
            waitMaxNs = Math.max(waitMaxNs, wait);
            matched++;
            pending = false;
        }
    }

    synchronized void rendered(long nativeNs) {
        renders++;
        nativeSumNs += nativeNs;
        nativeTimes.add(nativeNs);
        nativeMaxNs = Math.max(nativeMaxNs, nativeNs);
    }
    synchronized void callbackFinished(long ns) { callbackTimes.add(ns); }

    synchronized Snapshot snapshot(boolean setMark) {
        if (setMark) { mark = null; waits.reset(); nativeTimes.reset(); callbackTimes.reset(); }
        Snapshot s = new Snapshot(this);
        if (setMark) mark = s;
        return s;
    }

    static final class Snapshot {
        final long nowNs, camera, stylus, other, callbacks, withoutPending;
        final long matched, waitSumNs, waitMaxNs, renders, nativeSumNs, nativeMaxNs;
        final Snapshot baseline;
        final long[] waitWindow, nativeWindow, callbackWindow;

        private Snapshot(FrameTelemetry f) {
            nowNs = f.clock.getAsLong();
            camera = f.camera;
            stylus = f.stylus;
            other = f.other;
            callbacks = f.callbacks;
            matched = f.matched;
            withoutPending = callbacks - matched;
            waitSumNs = f.waitSumNs;
            waitMaxNs = f.waitMaxNs;
            renders = f.renders;
            nativeSumNs = f.nativeSumNs;
            nativeMaxNs = f.nativeMaxNs;
            baseline = f.mark;
            waitWindow=f.waits.summary(); nativeWindow=f.nativeTimes.summary();
            callbackWindow=f.callbackTimes.summary();
        }
    }
}
