package com.infinipaint.demo;

/** Single consumer. The last slot is reserved for the stroke's UP or CANCEL.
 * Tasks own their capture values, never MotionEvents. No task runs under the
 * queue monitor. The caller admits only one open gesture at a time. */
final class InkLane {
    private final Runnable[] ring;
    private int head, size, highWater;
    private long rejected;
    InkLane(int capacity) {
        if (capacity < 2) throw new IllegalArgumentException();
        ring = new Runnable[capacity];
    }
    synchronized boolean offer(Runnable work, boolean terminal) {
        if (size >= ring.length - (terminal ? 0 : 1)) {rejected++;return false;}
        ring[(head + size) % ring.length] = work;
        size++;
        highWater=Math.max(highWater,size);
        notify();
        return true;
    }
    synchronized long[] counters() {return new long[]{size,highWater,rejected};}
    private synchronized Runnable take() throws InterruptedException {
        while (size == 0) wait();
        Runnable work = ring[head];
        ring[head] = null;
        head = (head + 1) % ring.length;
        size--;
        return work;
    }
    void run() {
        try {
            while (!Thread.currentThread().isInterrupted()) take().run();
        } catch (InterruptedException stopped) {
            Thread.currentThread().interrupt();
        }
    }
}
