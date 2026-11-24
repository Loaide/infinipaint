package com.infinipaint.demo;

/** Tracks two pointer IDs, never their transient event indices. No per-event allocation. */
final class FingerNavigation<E> {
    interface Points<E> {
        int indexOf(E event, int id);
        double x(E event, int index);
        double y(E event, int index);
    }

    private final CanvasCamera camera;
    private final Points<E> points;
    private int first = -1, second = -1;
    private double x, y, span;

    FingerNavigation(CanvasCamera camera, Points<E> points) {
        this.camera = camera;
        this.points = points;
    }

    void cancel() { first = second = -1; }
    boolean pinching() { return second >= 0; }

    void down(E e, int id) {
        if (id < 0 || id == first || second >= 0) return;
        if (first < 0) first = id;
        else second = id;
        sample(e, false);
    }

    void up(E e, int id) {
        if (id != first && id != second) return;
        first = id == first ? second : first;
        second = -1;
        if (first >= 0) sample(e, false);
    }

    boolean move(E e) { return sample(e, true); }

    private boolean sample(E e, boolean move) {
        if (first < 0) return false;
        int a = points.indexOf(e, first);
        int b = second < 0 ? -1 : points.indexOf(e, second);
        if (a < 0 || (second >= 0 && b < 0)) {
            cancel();
            return false;
        }
        double nx = points.x(e, a), ny = points.y(e, a), ns = 1;
        if (b >= 0) {
            double bx = points.x(e, b), by = points.y(e, b);
            ns = Math.max(1, Math.hypot(nx - bx, ny - by));
            nx = (nx + bx) * 0.5;
            ny = (ny + by) * 0.5;
        }
        if (!Double.isFinite(nx) || !Double.isFinite(ny) || !Double.isFinite(ns)) {
            cancel();
            return false;
        }
        boolean changed = move && camera.track(x, y, nx, ny, ns / span);
        x = nx;
        y = ny;
        span = ns;
        return changed;
    }
}
