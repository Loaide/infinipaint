
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

import java.io.FileDescriptor;
import java.io.PrintWriter;
import java.util.Locale;

public final class MainActivity extends Activity {

    private static final String TAG = "IP90_INPUT";

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

    private final CanvasCamera camera = new CanvasCamera();

    // Stylus drawing state.
    private int stylusId = -1;

    // Finger navigation state.
    private final FingerNavigation<MotionEvent> fingers = new FingerNavigation<>(camera,
            new FingerNavigation.Points<MotionEvent>() {
                public int indexOf(MotionEvent e, int id) { return e.findPointerIndex(id); }
                public double x(MotionEvent e, int i) { return e.getX(i); }
                public double y(MotionEvent e, int i) { return e.getY(i); }
            });

    private long lastStylusHoverMs = Long.MIN_VALUE;
    private static final long HOVER_GRACE_MS = 500;

    private int frameCount = 0;
    private long fpsStart = 0;
    private long previousRenderEndNs;
    private volatile float measuredFps = 0;

    private boolean isStylus(int tool) {
        return tool == MotionEvent.TOOL_TYPE_STYLUS
                || tool == MotionEvent.TOOL_TYPE_ERASER;
    }

    private boolean isFinger(int tool) {
        return tool == MotionEvent.TOOL_TYPE_FINGER;
    }

    private final FrameTelemetry frames = new FrameTelemetry(System::nanoTime);
    // Event-driven: never render an unchanged canvas 90 times per second.
    private void requestCanvasRender(FrameTelemetry.Reason reason) {
        if (gl == null) return;
        frames.request(reason);
        gl.requestRender();
    }

    @Override
    public void dump(String prefix, FileDescriptor fd, PrintWriter out, String[] args) {
        boolean mark = args != null && args.length == 1 && "ip-frames-mark".equals(args[0]);
        if (!mark && (args == null || args.length != 1 || !"ip-frames".equals(args[0]))) {
            super.dump(prefix, fd, out, args);
            return;
        }
        FrameTelemetry.Snapshot s = frames.snapshot(mark);
        out.printf(Locale.US, "%sIP_FRAMES camera=%d stylus=%d other=%d callbacks=%d withoutPending=%d%n",
                prefix, s.camera, s.stylus, s.other, s.callbacks, s.withoutPending);
        out.printf(Locale.US, "%sfirst_pending_to_callback samples=%d sum_ns=%d max_ns=%d "
                        + "native_render samples=%d sum_ns=%d max_ns=%d%n",
                prefix, s.matched, s.waitSumNs, s.waitMaxNs, s.renders, s.nativeSumNs, s.nativeMaxNs);
        FrameTelemetry.Snapshot b = s.baseline;
        out.println(prefix+"recent_ns [count,p50,p95,p99,max] wait="+java.util.Arrays.toString(s.waitWindow)
                +" native="+java.util.Arrays.toString(s.nativeWindow)
                +" callback="+java.util.Arrays.toString(s.callbackWindow));
        if (b != null) {
            out.printf(Locale.US, "%ssince_mark elapsed_ms=%d camera=%d stylus=%d other=%d "
                            + "callbacks=%d withoutPending=%d%n",
                    prefix, (s.nowNs - b.nowNs) / 1_000_000, s.camera - b.camera,
                    s.stylus - b.stylus, s.other - b.other, s.callbacks - b.callbacks,
                    s.withoutPending - b.withoutPending);
        } else out.println(prefix + (mark ? "mark_set" : "no_mark"));
    }

    private void saveCamera(Bundle state) {
        state.putDouble("cameraPanX", camera.panX());
        state.putDouble("cameraPanY", camera.panY());
        state.putDouble("cameraZoom", camera.zoom());
    }

    private void restoreCamera(Bundle state) {
        camera.restore(state.getDouble("cameraPanX", 0), state.getDouble("cameraPanY", 0),
                state.getDouble("cameraZoom", 1));
    }

    private void updateCamera() {
        NativeBridge.setCamera(camera.panX(), camera.panY(), camera.zoom());
        requestCanvasRender(FrameTelemetry.Reason.CAMERA);
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
                camera.zoom(),
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
            requestCanvasRender(FrameTelemetry.Reason.STYLUS);
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
        requestCanvasRender(FrameTelemetry.Reason.STYLUS);
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
        requestCanvasRender(FrameTelemetry.Reason.STYLUS);
    }

    private void cancelStylus() {
        if (stylusId >= 0) {
            int id = stylusId;
            stylusId = -1;
            NativeBridge.cancel(id);
            requestCanvasRender(FrameTelemetry.Reason.STYLUS);
        }
        updateStatus();
    }

    // --------------------------------------------
    // Finger navigation
    // --------------------------------------------

    private void resetFingers() {
        fingers.cancel();
    }

    private boolean suppressFingers() {
        if (stylusId >= 0) return true;

        return lastStylusHoverMs != Long.MIN_VALUE
                && SystemClock.uptimeMillis()
                - lastStylusHoverMs < HOVER_GRACE_MS;
    }

    private void startFinger(MotionEvent e, int index) {
        if (suppressFingers()) return;
        if (!isFinger(e.getToolType(index))) return;
        fingers.down(e, e.getPointerId(index));
    }

    private void moveFingers(MotionEvent e) {
        if (suppressFingers()) {
            resetFingers();
            return;
        }

        if (fingers.move(e)) {
            updateCamera();
            if (fingers.pinching()) updateStatusThrottled();
        }
    }

    private void liftFinger(MotionEvent e, int index) {
        fingers.up(e, e.getPointerId(index));
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
        if (state != null) {
            mode = Math.max(0, Math.min(3, state.getInt("displayMode", 0)));
            smoothingPreset = Math.max(0, Math.min(2, state.getInt("smoothingPreset", 1)));
            restoreCamera(state);
        }
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

            @Override
            public void onSurfaceCreated(
                    javax.microedition.khronos.opengles.GL10 unused,
                    javax.microedition.khronos.egl.EGLConfig config) {

                NativeBridge.contextCreated();
            }

            @Override
            public void onSurfaceChanged(
                    javax.microedition.khronos.opengles.GL10 unused,
                    int w,
                    int h) {

                NativeBridge.resizeViewport(w, h);
                runOnUiThread(() -> request90Hz());
            }

            @Override
            public void onDrawFrame(
                    javax.microedition.khronos.opengles.GL10 unused) {

                long entryNs = System.nanoTime();
                frames.drawStarted();
                long startNs = System.nanoTime();
                NativeBridge.render(mode);
                long endNs = System.nanoTime();
                frames.rendered(endNs - startNs);
                // Restart the FPS measurement after idle; a paused canvas
                // should not make the next active stroke display 0 Hz.
                if (previousRenderEndNs == 0 || endNs - previousRenderEndNs > 2_000_000_000L) {
                    fpsStart = endNs;
                    frameCount = 0;
                } else frameCount++;
                previousRenderEndNs = endNs;
                if (endNs - fpsStart >= 1_000_000_000L) {
                    measuredFps = frameCount * 1e9f / (endNs - fpsStart);
                    frameCount = 0;
                    fpsStart = endNs;
                    runOnUiThread(() -> { if (running) updateStatus(); });
                }
                frames.callbackFinished(System.nanoTime()-entryNs);
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
            requestCanvasRender(FrameTelemetry.Reason.OTHER);
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
            requestCanvasRender(FrameTelemetry.Reason.OTHER);
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

        requestCanvasRender(FrameTelemetry.Reason.OTHER);
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

    @Override
    protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state);
        saveCamera(state);
        state.putInt("displayMode", mode);
        state.putInt("smoothingPreset", smoothingPreset);
    }

    @Override
    protected void onDestroy() {
        // Finish any pending diagnostic write; don't retain a worker per rotation.
        inputAudit.close();
        super.onDestroy();
    }
}
