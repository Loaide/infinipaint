package com.infinipaint.demo;

import java.util.function.LongSupplier;

/** Per-Activity counters. Only the explicit dump allocates; no lock spans rendering or I/O. */
final class FrameTelemetry {
    enum Reason { CAMERA, STYLUS, OTHER }

    private final LongSupplier clock;
    private long camera, stylus, other, callbacks, matched, firstNs, waitSumNs, waitMaxNs;
    private long renders, nativeSumNs, nativeMaxNs;
    private boolean pending;
    private Snapshot mark;

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
            waitMaxNs = Math.max(waitMaxNs, wait);
            matched++;
            pending = false;
        }
    }

    synchronized void rendered(long nativeNs) {
        renders++;
        nativeSumNs += nativeNs;
        nativeMaxNs = Math.max(nativeMaxNs, nativeNs);
    }

    synchronized Snapshot snapshot(boolean setMark) {
        if (setMark) mark = null;
        Snapshot s = new Snapshot(this);
        if (setMark) mark = s;
        return s;
    }

    static final class Snapshot {
        final long nowNs, camera, stylus, other, callbacks, withoutPending;
        final long matched, waitSumNs, waitMaxNs, renders, nativeSumNs, nativeMaxNs;
        final Snapshot baseline;

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
        }
    }
}
