
package com.infinipaint.demo;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.opengl.GLSurfaceView;
import android.util.Log;
import android.view.Display;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.Locale;

public final class MainActivity extends Activity {

    private static final String TAG = "IP90_INPUT";

    private static final double MIN_ZOOM = 0.1;
    private static final double MAX_ZOOM = 16.0;

    private GLSurfaceView gl;
    private TextView status;
    private Button modeButton;
    private Button smoothingButton;
    private int smoothingPreset = 1;
    private long lastStatusUpdateMs = -1;
    private InputAuditRecorder inputAudit;
    private long previousAuditTimestamp = Long.MIN_VALUE;
    private long auditLastCallbackNs = -1;

    private volatile int mode = 0;
    private boolean running = false;

    private double panX = 0.0;
    private double panY = 0.0;
    private double zoom = 1.0;

    // Stylus drawing state.
    private int stylusId = -1;

    // Finger navigation state.
    private int firstFingerId = -1;
    private int secondFingerId = -1;

    private float previousX;
    private float previousY;
    private float previousCx;
    private float previousCy;
    private float previousDistance;

    private long lastStylusHoverMs = Long.MIN_VALUE;
    private static final long HOVER_GRACE_MS = 500;

    private int frameCount = 0;
    private long fpsStart = 0;
    private volatile float measuredFps = 0;

    private boolean isStylus(int tool) {
        return tool == MotionEvent.TOOL_TYPE_STYLUS
                || tool == MotionEvent.TOOL_TYPE_ERASER;
    }

    private boolean isFinger(int tool) {
        return tool == MotionEvent.TOOL_TYPE_FINGER;
    }

    private volatile long lastRequestedFrameNs;
    // Event-driven: never render an unchanged canvas 90 times per second.
    private void requestCanvasRender() {
        if (gl == null) return;
        lastRequestedFrameNs = System.nanoTime();
        gl.requestRender();
    }

    private void updateCamera() {
        NativeBridge.setCamera(panX, panY, zoom);
        requestCanvasRender();
    }

    private void updateStatusThrottled() {
        long now = SystemClock.uptimeMillis();
        if (lastStatusUpdateMs < 0 || now - lastStatusUpdateMs >= 125) {
            lastStatusUpdateMs = now;
            updateStatus();
        }
    }

    private void updateStatus() {
        if (status == null) return;

        status.setText(String.format(
                Locale.US,
                "Zoom %.2fx | Render %.0f Hz | %s",
                zoom,
                measuredFps,
                stylusId >= 0 ? "STYLUS DRAWING" : "READY"
        ));
    }

    private void updateModeButton() {
        if (modeButton != null) {
            modeButton.setText(new String[]{"Raw", "One-Euro", "Spline", "Compare"}[mode]);
        }
    }

    private void updateSmoothingButton() {
        if (smoothingButton != null)
            smoothingButton.setText(new String[]{"Smooth: Light", "Smooth: Medium", "Smooth: Strong"}[smoothingPreset]);
    }

    // --------------------------------------------
    // Stylus handling
    // --------------------------------------------

    private void startStylus(MotionEvent e, int index) {
        if (stylusId >= 0) return;

        int id = e.getPointerId(index);
        int tool = e.getToolType(index);

        if (!isStylus(tool)) return;

        Log.i(TAG, "Stylus DOWN, tool=" + tool
                + ", id=" + id);

        if (NativeBridge.begin(id, tool)) {
            previousAuditTimestamp = Long.MIN_VALUE;
            stylusId = id;

            // Ignore navigation contacts during drawing.
            resetFingers();

            sendStylusSample(e, index, -1);
            requestCanvasRender();
            updateStatus();
        } else {
            Log.e(TAG, "NativeBridge.begin rejected stylus");
        }
    }

    private void sendStylusSample(
            MotionEvent e,
            int index,
            int history
    ) {
        if (stylusId < 0) return;

        float x = history < 0
                ? e.getX(index)
                : e.getHistoricalX(index, history);

        float y = history < 0
                ? e.getY(index)
                : e.getHistoricalY(index, history);

        float pressure = history < 0
                ? e.getPressure(index)
                : e.getHistoricalPressure(index, history);

        float tilt = history < 0
                ? e.getAxisValue(MotionEvent.AXIS_TILT, index)
                : e.getHistoricalAxisValue(
                MotionEvent.AXIS_TILT, index, history);

        float orientation = history < 0
                ? e.getOrientation(index)
                : e.getHistoricalOrientation(index, history);

        // API 34+ preserves nanosecond event resolution. Earlier API levels
        // preserve the original milliseconds; never fabricate timestamps.
        long timestampNs;
        if (Build.VERSION.SDK_INT >= 34) {
            timestampNs = history < 0 ? e.getEventTimeNanos()
                    : e.getHistoricalEventTimeNanos(history);
        } else {
            timestampNs = (history < 0 ? e.getEventTime()
                    : e.getHistoricalEventTime(history)) * 1_000_000L;
        }
        double seconds = timestampNs / 1_000_000_000.0;

        float tiltX =
                (float) (tilt * Math.sin(orientation));

        float tiltY =
                (float) (tilt * Math.cos(orientation));

        if (!Float.isFinite(x)
                || !Float.isFinite(y)
                || !Float.isFinite(pressure)
                || !Float.isFinite(tiltX)
                || !Float.isFinite(tiltY)) {
            cancelStylus();
            return;
        }

        // Record every raw Android sample before the native processing path.
        final long sampleNs = timestampNs;
        final long callbackNs = SystemClock.uptimeMillis() * 1_000_000L;
        final String outcome;
        if (sampleNs < previousAuditTimestamp) outcome = "OUT_OF_ORDER";
        else if (sampleNs == previousAuditTimestamp) outcome = "DUPLICATE_TIMESTAMP";
        else outcome = "PENDING";
        previousAuditTimestamp = sampleNs;
        boolean accepted = NativeBridge.sample(
                stylusId,
                x,
                y,
                seconds,
                Math.max(0f, Math.min(1f, pressure)),
                tiltX,
                tiltY
        );

        if (inputAudit != null) inputAudit.record(stylusId, e.getToolType(index), sampleNs,
                callbackNs, x, y, pressure, tiltX, tiltY, history >= 0,
                accepted ? ("DUPLICATE_TIMESTAMP".equals(outcome) ? "ACCEPTED_SAME_TIMESTAMP" : "PENDING".equals(outcome) ? "ACCEPTED" : outcome) : "NATIVE_REJECTED");
        if (!accepted) {
            Log.e(TAG, "NativeBridge.sample rejected input");
            cancelStylus();
        }
    }

    private void moveStylus(MotionEvent e) {
        if (stylusId < 0) return;

        int index = e.findPointerIndex(stylusId);

        if (index < 0) {
            cancelStylus();
            return;
        }

        for (int h = 0; h < e.getHistorySize(); h++) {
            sendStylusSample(e, index, h);
            if (stylusId < 0) return;
        }

        sendStylusSample(e, index, -1);
        // One render request per MotionEvent, not per historical sample.
        requestCanvasRender();
    }

    private void finishStylus(MotionEvent e, int index) {
        if (stylusId < 0) return;
        if (e.getPointerId(index) != stylusId) return;

        int id = stylusId;

        // Include the final tip position.
        sendStylusSample(e, index, -1);

        if (stylusId != id) return;

        stylusId = -1;

        if (!NativeBridge.end(id)) {
            Log.e(TAG, "NativeBridge.end failed");
            NativeBridge.cancel(id);
        }

        Log.i(TAG, "Stylus UP");

        resetFingers();
        updateStatus();
        requestCanvasRender();
    }

    private void cancelStylus() {
        if (stylusId >= 0) {
            int id = stylusId;
            stylusId = -1;
            NativeBridge.cancel(id);
            requestCanvasRender();
        }
        updateStatus();
    }

    // --------------------------------------------
    // Finger navigation
    // --------------------------------------------

    private void resetFingers() {
        firstFingerId = -1;
        secondFingerId = -1;
    }

    private boolean suppressFingers() {
        if (stylusId >= 0) return true;

        return lastStylusHoverMs != Long.MIN_VALUE
                && SystemClock.uptimeMillis()
                - lastStylusHoverMs < HOVER_GRACE_MS;
    }

    private boolean getPinch(
            MotionEvent e,
            float[] out
    ) {
        int a = e.findPointerIndex(firstFingerId);
        int b = e.findPointerIndex(secondFingerId);

        if (a < 0 || b < 0) return false;

        float ax = e.getX(a);
        float ay = e.getY(a);

        float bx = e.getX(b);
        float by = e.getY(b);

        out[0] = (ax + bx) * 0.5f;
        out[1] = (ay + by) * 0.5f;
        out[2] = (float) Math.hypot(
                bx - ax,
                by - ay
        );

        return true;
    }

    private final float[] pinch = new float[3];

    private void startFinger(MotionEvent e, int index) {
        if (suppressFingers()) return;
        if (!isFinger(e.getToolType(index))) return;

        int id = e.getPointerId(index);

        if (firstFingerId < 0) {
            firstFingerId = id;
            previousX = e.getX(index);
            previousY = e.getY(index);
            return;
        }

        if (secondFingerId < 0 && id != firstFingerId) {
            secondFingerId = id;

            if (getPinch(e, pinch)) {
                previousCx = pinch[0];
                previousCy = pinch[1];
                previousDistance = Math.max(1f, pinch[2]);
            }
        }
    }

    private void moveFingers(MotionEvent e) {
        if (suppressFingers()) {
            resetFingers();
            return;
        }

        if (firstFingerId < 0) return;

        if (secondFingerId >= 0) {
            if (!getPinch(e, pinch)) {
                resetFingers();
                return;
            }

            float cx = pinch[0];
            float cy = pinch[1];
            float distance = Math.max(1f, pinch[2]);

            double oldZoom = zoom;
            double factor = distance / previousDistance;

            double newZoom = Math.max(
                    MIN_ZOOM,
                    Math.min(MAX_ZOOM, oldZoom * factor)
            );

            // Keep the same world location under the
            // moving midpoint of the two fingers.
            panX += previousCx / oldZoom - cx / newZoom;
            panY += previousCy / oldZoom - cy / newZoom;
            zoom = newZoom;

            previousCx = cx;
            previousCy = cy;
            previousDistance = distance;

            updateCamera();
            updateStatusThrottled();
            return;
        }

        int index = e.findPointerIndex(firstFingerId);

        if (index < 0) {
            resetFingers();
            return;
        }

        float x = e.getX(index);
        float y = e.getY(index);

        // One-finger pan.
        panX -= (x - previousX) / zoom;
        panY -= (y - previousY) / zoom;

        previousX = x;
        previousY = y;

        updateCamera();
    }

    private void liftFinger(MotionEvent e, int index) {
        int liftedId = e.getPointerId(index);

        if (liftedId != firstFingerId
                && liftedId != secondFingerId) {
            return;
        }

        // Re-anchor the remaining finger, avoiding jumps.
        int remainingId = -1;

        for (int i = 0; i < e.getPointerCount(); i++) {
            if (i == index) continue;

            if (isFinger(e.getToolType(i))) {
                remainingId = e.getPointerId(i);
                previousX = e.getX(i);
                previousY = e.getY(i);
                break;
            }
        }

        firstFingerId = remainingId;
        secondFingerId = -1;
    }

    // --------------------------------------------
    // Touch routing
    // --------------------------------------------

    private boolean handleTouch(MotionEvent e) {
        int action = e.getActionMasked();
        int index = e.getActionIndex();

        if (action == MotionEvent.ACTION_CANCEL) {
            cancelStylus();
            resetFingers();
            return true;
        }

        if (action == MotionEvent.ACTION_DOWN
                || action == MotionEvent.ACTION_POINTER_DOWN) {

            int tool = e.getToolType(index);

            Log.i(TAG,
                    "DOWN tool=" + tool
                            + " pointer=" + e.getPointerId(index));

            if (isStylus(tool)) {
                startStylus(e, index);
            } else if (isFinger(tool)) {
                startFinger(e, index);
            }

            return true;
        }

        if (action == MotionEvent.ACTION_MOVE) {

            if (stylusId >= 0) {
                moveStylus(e);
            } else {
                moveFingers(e);
            }

            return true;
        }

        if (action == MotionEvent.ACTION_UP
                || action == MotionEvent.ACTION_POINTER_UP) {

            int id = e.getPointerId(index);

            if (id == stylusId) {
                finishStylus(e, index);
            } else {
                liftFinger(e, index);
            }

            return true;
        }

        return true;
    }

    // --------------------------------------------
    // Display refresh request
    // --------------------------------------------

    private void request90Hz() {
        if (Build.VERSION.SDK_INT >= 23) {
            Display d = getWindowManager().getDefaultDisplay();
            Display.Mode current = d.getMode();

            Display.Mode best = null;
            float diff = Float.MAX_VALUE;

            for (Display.Mode m : d.getSupportedModes()) {
                if (m.getPhysicalWidth()
                        != current.getPhysicalWidth()
                        || m.getPhysicalHeight()
                        != current.getPhysicalHeight()) {
                    continue;
                }

                float delta =
                        Math.abs(m.getRefreshRate() - 90f);

                if (delta < diff) {
                    diff = delta;
                    best = m;
                }
            }

            if (best != null && diff < 2f) {
                WindowManager.LayoutParams p =
                        getWindow().getAttributes();

                p.preferredDisplayModeId = best.getModeId();
                getWindow().setAttributes(p);
            }
        }

        if (Build.VERSION.SDK_INT >= 30
                && gl.getHolder().getSurface().isValid()) {
            gl.getHolder().getSurface().setFrameRate(
                    90f,
                    Surface.FRAME_RATE_COMPATIBILITY_DEFAULT
            );
        }
    }

    // --------------------------------------------
    // Activity lifecycle
    // --------------------------------------------

    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);
        inputAudit = new InputAuditRecorder(this);

        getWindow().addFlags(
                WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
        );

        // Use normal system window insets:
        // no fullscreen or edge-to-edge toolbar overlap.
        getWindow().getDecorView().setSystemUiVisibility(0);

        FrameLayout root = new FrameLayout(this);

        gl = new GLSurfaceView(this);
        gl.setEGLContextClientVersion(2);
        gl.setPreserveEGLContextOnPause(true);

        gl.setRenderer(new GLSurfaceView.Renderer() {

            // Zero-initialized per renderer; only the GL thread updates this window.
            private long renderWindowQueueNs, renderWindowNativeNs;
            private int renderWindowQueueSamples, renderWindowCount;

            @Override
            public void onSurfaceCreated(
                    javax.microedition.khronos.opengles.GL10 unused,
                    javax.microedition.khronos.egl.EGLConfig config) {

                // onSurfaceChanged follows this callback and initializes the
                // renderer once at the actual viewport size.
            }

            @Override
            public void onSurfaceChanged(
                    javax.microedition.khronos.opengles.GL10 unused,
                    int w,
                    int h) {

                NativeBridge.surface(w, h);
                runOnUiThread(() -> request90Hz());
            }

            @Override
            public void onDrawFrame(
                    javax.microedition.khronos.opengles.GL10 unused) {

                long startNs = System.nanoTime();
                NativeBridge.render(mode);
                long endNs = System.nanoTime();
                // Restart the FPS measurement after idle; a paused canvas
                // should not make the next active stroke display 0 Hz.
                if (fpsStart == 0 || endNs - fpsStart > 2_000_000_000L) {
                    fpsStart = endNs;
                    frameCount = 0;
                }
                frameCount++;
                if (endNs - fpsStart >= 1_000_000_000L) {
                    measuredFps = frameCount * 1e9f / (endNs - fpsStart);
                    frameCount = 0;
                    fpsStart = endNs;
                    runOnUiThread(() -> { if (running) updateStatus(); });
                }
                long requestedNs = lastRequestedFrameNs;
                if (requestedNs > 0 && startNs >= requestedNs && startNs - requestedNs < 100000000L) {
                    renderWindowQueueNs += startNs - requestedNs;
                    renderWindowQueueSamples++;
                }
                renderWindowNativeNs += endNs - startNs;
                if (++renderWindowCount == 120) {
                    android.util.Log.i("IP_FRAME_TIMING", String.format(java.util.Locale.US,
                        "event_request_to_draw_avg_ms=%.3f native_render_avg_ms=%.3f queue_samples=%d " +
                        "note=GLSurfaceView swaps after onDrawFrame; actual presentation not measured",
                        renderWindowQueueSamples > 0 ? renderWindowQueueNs / (renderWindowQueueSamples*1e6) : -1.0,
                        renderWindowNativeNs / 120e6, renderWindowQueueSamples));
                    renderWindowQueueNs = renderWindowNativeNs = renderWindowQueueSamples = 0;
                    renderWindowCount = 0;
                }
            }
        });

        gl.setRenderMode(
                GLSurfaceView.RENDERMODE_WHEN_DIRTY
        );

        gl.setOnTouchListener((v, e) -> handleTouch(e));

        gl.setOnHoverListener((v, e) -> {
            int action = e.getActionMasked();

            if (action == MotionEvent.ACTION_HOVER_ENTER
                    || action == MotionEvent.ACTION_HOVER_MOVE) {

                for (int i = 0; i < e.getPointerCount(); i++) {
                    if (isStylus(e.getToolType(i))) {
                        lastStylusHoverMs =
                                SystemClock.uptimeMillis();
                        break;
                    }
                }
            }

            return true;
        });

        root.addView(gl, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT
        ));

        // Toolbar occupies its own space at the top.
        LinearLayout toolbar = new LinearLayout(this);
        toolbar.setOrientation(LinearLayout.HORIZONTAL);
        toolbar.setGravity(android.view.Gravity.CENTER_VERTICAL);
        toolbar.setBackgroundColor(0xff18232e);
        toolbar.setPadding(12, 8, 12, 8);

        status = new TextView(this);
        status.setTextColor(0xffffffff);
        status.setTextSize(13);
        status.setText("InfiniPaint");
        status.setPadding(8, 8, 8, 8);

        toolbar.addView(status,
                new LinearLayout.LayoutParams(
                        0,
                        LinearLayout.LayoutParams.WRAP_CONTENT,
                        1f
                ));

        modeButton = new Button(this);
        modeButton.setText("Mode 0");

        modeButton.setOnClickListener(v -> {
            mode = (mode + 1) % 4;
            updateModeButton();
            requestCanvasRender();
        });

        toolbar.addView(modeButton);

        smoothingButton = new Button(this);
        smoothingButton.setOnClickListener(v -> {
            smoothingPreset = (smoothingPreset + 1) % 3;
            NativeBridge.setSmoothingPreset(smoothingPreset);
            updateSmoothingButton();
            // Preset is latched at pen-down; existing strokes are unchanged.
            android.widget.Toast.makeText(this, "Smoothing applies to next stroke", android.widget.Toast.LENGTH_SHORT).show();
        });
        updateSmoothingButton();
        toolbar.addView(smoothingButton);

        Button clearButton = new Button(this);
        clearButton.setText("Clear");

        clearButton.setOnLongClickListener(v -> {
            inputAudit.export();
            android.widget.Toast.makeText(this, "Raw samples exported; see IP_INPUT_AUDIT in Logcat", android.widget.Toast.LENGTH_LONG).show();
            return true;
        });
        clearButton.setOnClickListener(v -> {
            cancelStylus();
            resetFingers();
            NativeBridge.clear();
            requestCanvasRender();
        });

        toolbar.addView(clearButton);

        // Put the toolbar in normal layout space.
        // This prevents it from covering the drawing surface
        // or being obscured by the Android status bar.
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.addView(toolbar);

        FrameLayout canvasContainer = new FrameLayout(this);

        // Move GL view from root into the canvas container.
        root.removeView(gl);
        canvasContainer.addView(gl);

        layout.addView(
                canvasContainer,
                new LinearLayout.LayoutParams(
                        LinearLayout.LayoutParams.MATCH_PARENT,
                        0,
                        1f
                )
        );

        setContentView(layout);

        NativeBridge.setSmoothingPreset(smoothingPreset);
        updateModeButton();
        updateCamera();
        updateStatus();
    }

    @Override
    protected void onResume() {
        super.onResume();

        gl.onResume();
        running = true;

        request90Hz();

        requestCanvasRender();
    }

    @Override
    protected void onPause() {
        if (inputAudit != null) inputAudit.export();
        running = false;

        cancelStylus();
        resetFingers();

        gl.onPause();
        super.onPause();
    }
}
