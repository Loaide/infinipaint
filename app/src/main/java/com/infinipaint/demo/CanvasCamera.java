package com.infinipaint.demo;

/** UI-thread camera. Screen pixels are relative to the viewport's top-left corner. */
final class CanvasCamera {
    static final double MIN_ZOOM = 0.1, MAX_ZOOM = 16.0;
    private double x, y, scale = 1.0;

    double panX() { return x; }
    double panY() { return y; }
    double zoom() { return scale; }

    boolean restore(double px, double py, double z) {
        if (!Double.isFinite(px) || !Double.isFinite(py)
                || !(z >= MIN_ZOOM && z <= MAX_ZOOM)) return false;
        x = px;
        y = py;
        scale = z;
        return true;
    }

    /** Move the world point under (oldX, oldY) to (newX, newY) at the clamped scale. */
    boolean track(double oldX, double oldY, double newX, double newY, double ratio) {
        if (!(ratio > 0) || !Double.isFinite(ratio)) return false;
        double z = Math.max(MIN_ZOOM, Math.min(MAX_ZOOM, scale * ratio));
        // Work in screen deltas before adding the potentially large world origin.
        double dx = z == scale ? (oldX - newX) / z : oldX / scale - newX / z;
        double dy = z == scale ? (oldY - newY) / z : oldY / scale - newY / z;
        double px = x + dx, py = y + dy;
        return (px != x || py != y || z != scale) && restore(px, py, z);
    }
}
