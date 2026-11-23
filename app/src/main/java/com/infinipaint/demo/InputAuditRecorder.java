package com.infinipaint.demo;

import android.content.Context;
import android.util.Log;
import java.io.*;
import java.util.*;
import java.util.concurrent.*;

/** Diagnostic-only, append-only original MotionEvent sample capture. No GPU dependency. */
public final class InputAuditRecorder {
    private static final int MAX_ROWS = 250000;
    private final ArrayList<String> rows = new ArrayList<>();
    private final ExecutorService io = Executors.newSingleThreadExecutor();
    private final File dir;
    private long accepted, historical, duplicates, rejected, outOfOrder, overflow;
    private long lastAcceptedNs = -1, minIntervalNs = Long.MAX_VALUE, maxIntervalNs;
    private double sumIntervalMs;
    private long intervals;
    private float minPressure = Float.POSITIVE_INFINITY, maxPressure = Float.NEGATIVE_INFINITY;
    public InputAuditRecorder(Context context) {
        File base = context.getExternalFilesDir("diagnostics");
        dir = base != null ? base : new File(context.getFilesDir(), "diagnostics");
        if (!dir.exists() && !dir.mkdirs()) Log.e("IP_INPUT_AUDIT", "Cannot create " + dir);
    }
    public void record(int id, int tool, long sampleNs, long callbackNs, double x, double y,
                       float pressure, float tx, float ty, boolean historicalSample, String outcome) {
        // Called on UI thread. No disk I/O, no per-sample logging.
        if (rows.size() >= MAX_ROWS) { overflow++; return; }
        // String.format uses a heavyweight formatter on the touch callback.
        // A small builder avoids formatter allocation per hardware sample.
        rows.add(new StringBuilder(120).append(id).append(',').append(tool)
                .append(',').append(sampleNs).append(',').append(callbackNs)
                .append(',').append(x).append(',').append(y)
                .append(',').append(pressure).append(',').append(tx).append(',').append(ty)
                .append(',').append(historicalSample ? 1 : 0).append(',').append(outcome)
                .toString());
        if (historicalSample) historical++;
        if ("ACCEPTED".equals(outcome) || "ACCEPTED_SAME_TIMESTAMP".equals(outcome)) {
            accepted++;
            minPressure = Math.min(minPressure, pressure);
            maxPressure = Math.max(maxPressure, pressure);
            if (lastAcceptedNs >= 0 && sampleNs > lastAcceptedNs) {
                long dt = sampleNs - lastAcceptedNs;
                minIntervalNs = Math.min(minIntervalNs, dt);
                maxIntervalNs = Math.max(maxIntervalNs, dt);
                sumIntervalMs += dt / 1000000.0;
                intervals++;
            }
            lastAcceptedNs = sampleNs;
        } else if ("DUPLICATE_TIMESTAMP".equals(outcome)) duplicates++;
        else if ("OUT_OF_ORDER".equals(outcome)) outOfOrder++;
        else rejected++;
    }
    public void export() {
        if (rows.isEmpty()) return;
        final ArrayList<String> batch = new ArrayList<>(rows);
        rows.clear();
        final String name = "ip_raw_" + System.currentTimeMillis() + ".csv";
        final File target = new File(dir, name);
        final String summary = String.format(Locale.US,
                "accepted=%d historical=%d duplicate_skipped=%d out_of_order=%d rejected=%d overflow=%d " +
                "dt_avg_ms=%.3f dt_min_ms=%.3f dt_max_ms=%.3f pressure_min=%.3f pressure_max=%.3f",
                accepted, historical, duplicates, outOfOrder, rejected, overflow,
                intervals > 0 ? sumIntervalMs / intervals : -1,
                intervals > 0 ? minIntervalNs / 1e6 : -1,
                intervals > 0 ? maxIntervalNs / 1e6 : -1,
                accepted > 0 ? minPressure : -1, accepted > 0 ? maxPressure : -1);
        accepted=historical=duplicates=rejected=outOfOrder=overflow=intervals=0;
        lastAcceptedNs=-1; minIntervalNs=Long.MAX_VALUE; maxIntervalNs=0;
        sumIntervalMs=0; minPressure=Float.POSITIVE_INFINITY; maxPressure=Float.NEGATIVE_INFINITY;
        io.execute(() -> {
            try (BufferedWriter w = new BufferedWriter(new FileWriter(target))) {
                w.write("pointer_id,tool,sample_ns,callback_ns,x_px,y_px,pressure,tilt_x,tilt_y,historical,outcome\n");
                for (String row : batch) { w.write(row); w.newLine(); }
                Log.i("IP_INPUT_AUDIT", summary + " file=" + target.getAbsolutePath());
            } catch (IOException e) { Log.e("IP_INPUT_AUDIT", "Export failed", e); }
        });
    }
    public void close() { export(); io.shutdown(); }
}
