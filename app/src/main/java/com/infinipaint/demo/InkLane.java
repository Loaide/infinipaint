package com.infinipaint.demo;

/** Single consumer. The last slot is reserved for the stroke's UP or CANCEL.
 * Tasks own their capture values, never MotionEvents. No task runs under the
 * queue monitor. The caller admits only one open gesture at a time. */
final class InkLane {
    private final Runnable[] ring;
    private int head, size, highWater;
    private long rejected;
    // One out-of-band resolution slot. Only the consumer pauses/resumes;
    // the producer can enqueue a resolution while the FIFO is parked.
    private boolean paused;
    private Runnable resolution;
    synchronized void pause() {paused=true;}
    synchronized void resume() {paused=false;notifyAll();}
    synchronized boolean resolve(Runnable work) {
        if(!paused || resolution!=null) return false;
        resolution=work;notifyAll();return true;
    }
    synchronized boolean isPaused() {return paused;}
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
        while (paused ? resolution==null : size==0) wait();
        if(paused) {
            Runnable work=resolution; resolution=null;return work;
        }
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
