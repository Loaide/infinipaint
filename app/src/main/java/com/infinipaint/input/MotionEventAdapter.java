package com.infinipaint.input;

import android.view.MotionEvent;
import android.os.Build;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;

/** View-local input adapter. Confine calls to one UI thread; callbacks must not re-enter.
 * Sink returns false on native failure; the affected pointer is cancelled immediately.
 * A single processor must be created per pointer ID. No JNI or renderer dependency here.
 */
public final class MotionEventAdapter {
    public interface Sink {
        boolean begin(int pointerId, int toolType);
        boolean sample(int pointerId, double xPx, double yPx, double seconds,
                       float pressure, float tiltX, float tiltY);
        boolean end(int pointerId);
        void cancel(int pointerId);
    }
    private final Sink sink;
    private final Map<Integer, Long> lastTimeNanos = new HashMap<>();
    private boolean dispatching = false;

    public MotionEventAdapter(Sink sink) {
        if (sink == null) throw new IllegalArgumentException("sink");
        this.sink = sink;
    }

    private void cancelPointer(int id) {
        if (lastTimeNanos.remove(id) != null) sink.cancel(id);
    }

    private void begin(MotionEvent event, int index) {
        int id = event.getPointerId(index);
        cancelPointer(id);
        if (!sink.begin(id, event.getToolType(index))) {
            sink.cancel(id);
            return;
        }
        lastTimeNanos.put(id, Long.MIN_VALUE);
        emit(event, index, -1);
    }

    private void emit(MotionEvent event, int index, int historyIndex) {
        int id = event.getPointerId(index);
        Long previous = lastTimeNanos.get(id);
        if (previous == null) return;
        // Nanosecond event times are available on newer Android releases.
        // Older releases expose milliseconds only; do not invent sub-ms times.
        long ns;
        if (Build.VERSION.SDK_INT >= 34) {
            ns = historyIndex < 0 ? event.getEventTimeNanos()
                    : event.getHistoricalEventTimeNanos(historyIndex);
        } else {
            long ms = historyIndex < 0 ? event.getEventTime()
                    : event.getHistoricalEventTime(historyIndex);
            if (ms < 0 || ms > Long.MAX_VALUE / 1000000L) { cancelPointer(id); return; }
            ns = ms * 1000000L;
        }
        if (ns < previous) { cancelPointer(id); return; }
        // Same-time samples may have distinct positions/pressure: forward them.
        float x = historyIndex < 0 ? event.getX(index) : event.getHistoricalX(index, historyIndex);
        float y = historyIndex < 0 ? event.getY(index) : event.getHistoricalY(index, historyIndex);
        float pressure = historyIndex < 0 ? event.getPressure(index) : event.getHistoricalPressure(index, historyIndex);
        float tilt = historyIndex < 0 ? event.getAxisValue(MotionEvent.AXIS_TILT, index) : event.getHistoricalAxisValue(MotionEvent.AXIS_TILT, index, historyIndex);
        float orientation = historyIndex < 0 ? event.getOrientation(index) : event.getHistoricalOrientation(index, historyIndex);
        if (!Float.isFinite(x) || !Float.isFinite(y) || !Float.isFinite(pressure)
                || !Float.isFinite(tilt) || !Float.isFinite(orientation)) {
            cancelPointer(id);
            return;
        }
        float tx = (float)(tilt * Math.sin(orientation));
        float ty = (float)(tilt * Math.cos(orientation));
        if (!Float.isFinite(tx) || !Float.isFinite(ty) || ns < 0) {
            cancelPointer(id);
            return;
        }
        boolean accepted = sink.sample(id, x, y, ns / 1000000000.0,
                Math.max(0f, Math.min(1f, pressure)), tx, ty);
        if (accepted) lastTimeNanos.put(id, ns);
        else cancelPointer(id);
    }

    private void history(MotionEvent event) {
        for (int h = 0; h < event.getHistorySize(); ++h)
            for (int i = 0; i < event.getPointerCount(); ++i) emit(event, i, h);
    }

    private void current(MotionEvent event) {
        for (int i = 0; i < event.getPointerCount(); ++i) emit(event, i, -1);
    }

    private void cancelAllInternal() {
        ArrayList<Integer> ids = new ArrayList<>(lastTimeNanos.keySet());
        lastTimeNanos.clear();
        for (int id : ids) sink.cancel(id);
    }

    public void cancelAll() {
        if (dispatching) throw new IllegalStateException("Reentrant input dispatch");
        dispatching = true;
        try { cancelAllInternal(); }
        finally { dispatching = false; }
    }

    public boolean onTouchEvent(MotionEvent event) {
        if (event == null) throw new IllegalArgumentException("event");
        if (dispatching) throw new IllegalStateException("Reentrant input dispatch");
        dispatching = true;
        try {
            int action = event.getActionMasked();
            int index = event.getActionIndex();
            switch (action) {
                case MotionEvent.ACTION_DOWN:
                case MotionEvent.ACTION_POINTER_DOWN:
                    history(event);
                    // Existing pointers may have new positions in a pointer-down event.
                    for (int i = 0; i < event.getPointerCount(); ++i)
                        if (i != index) emit(event, i, -1);
                    begin(event, index);
                    return true;
                case MotionEvent.ACTION_MOVE:
                    history(event);
                    current(event);
                    return true;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_POINTER_UP:
                    history(event);
                    // Preserve samples for pointers remaining down as well as the lifted one.
                    current(event);
                    int id = event.getPointerId(index);
                    if (lastTimeNanos.remove(id) != null && !sink.end(id)) sink.cancel(id);
                    return true;
                case MotionEvent.ACTION_CANCEL:
                    cancelAllInternal();
                    return true;
                default:
                    return false;
            }
        } catch (RuntimeException | Error failure) {
            // Best effort cancellation before propagating an unexpected host failure.
            try { cancelAllInternal(); } catch (RuntimeException | Error ignored) { failure.addSuppressed(ignored); }
            throw failure;
        } finally {
            dispatching = false;
        }
    }
}
