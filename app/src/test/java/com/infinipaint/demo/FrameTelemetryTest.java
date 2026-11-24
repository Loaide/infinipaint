package com.infinipaint.demo;

import java.util.concurrent.atomic.AtomicLong;
import org.junit.Test;
import static org.junit.Assert.*;

public class FrameTelemetryTest {
    private final AtomicLong now = new AtomicLong();
    private final FrameTelemetry frames = new FrameTelemetry(now::get);

    @Test public void coalescedRequestsMeasureOldestOnceIncludingSlowFrames() {
        frames.request(FrameTelemetry.Reason.CAMERA);
        now.set(110_000_000);
        frames.request(FrameTelemetry.Reason.STYLUS);
        now.set(150_000_000);
        frames.drawStarted();
        // A request arriving during native render belongs to the next callback.
        frames.request(FrameTelemetry.Reason.OTHER);
        frames.rendered(9);
        now.addAndGet(25);
        frames.drawStarted();
        frames.rendered(5);
        frames.drawStarted();
        FrameTelemetry.Snapshot s = frames.snapshot(false);
        assertEquals(1, s.camera);
        assertEquals(1, s.stylus);
        assertEquals(1, s.other);
        assertEquals(3, s.callbacks);
        assertEquals(1, s.withoutPending);
        assertEquals(2, s.matched);
        assertEquals(150_000_025, s.waitSumNs);
        assertEquals(150_000_000, s.waitMaxNs);
        assertEquals(2, s.renders);
        assertEquals(14, s.nativeSumNs);
        assertEquals(9, s.nativeMaxNs);
    }

    @Test public void marksDoNotDrawAndIdleDeltasIncludePlatformCallbacks() {
        frames.drawStarted();
        frames.snapshot(true);
        now.set(30_000_000_000L);
        FrameTelemetry.Snapshot idle = frames.snapshot(false);
        assertEquals(idle.baseline.callbacks, idle.callbacks);
        assertEquals(30_000_000_000L, idle.nowNs - idle.baseline.nowNs);
        frames.drawStarted();
        FrameTelemetry.Snapshot after = frames.snapshot(false);
        assertEquals(1, after.callbacks - after.baseline.callbacks);
        assertEquals(1, after.withoutPending - after.baseline.withoutPending);
        assertEquals(0, after.camera + after.stylus + after.other);
        assertNull(frames.snapshot(true).baseline);
        assertEquals(2, frames.snapshot(false).baseline.callbacks);
    }
}
