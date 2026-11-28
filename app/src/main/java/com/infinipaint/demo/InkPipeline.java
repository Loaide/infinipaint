package com.infinipaint.demo;

/** Process-scoped ordered capture lane. A single producer admits one open
 * gesture at a time, but may queue subsequent closed gestures before the
 * worker seals earlier ones. UI acknowledgements bound retained callbacks.
 * A failed native admission/commit parks the FIFO; one explicit resolution
 * may pass the blocked head. No later native DOWN runs against a live prefix.
 */
final class InkPipeline {
    enum Phase { BEGIN, END }
    enum Outcome { COMMITTED, CANCELLED, DISCARDED }
    static final class Capture {
        final int id,tool,preset;
        final double x,y,zoom;
        Capture(int id,int tool,double x,double y,double zoom,int preset) {
            this.id=id;this.tool=tool;this.x=x;this.y=y;this.zoom=zoom;this.preset=preset;
        }
    }
    static final class Sample {
        final double x,y,seconds;
        final float pressure,tx,ty;
        Sample(double x,double y,double seconds,float pressure,float tx,float ty) {
            this.x=x;this.y=y;this.seconds=seconds;this.pressure=pressure;this.tx=tx;this.ty=ty;
        }
    }
    interface Engine {
        boolean begin(Capture capture);
        boolean sample(int id,Sample sample);
        boolean end(int id);
        void cancel(int id);
    }
    interface Observer {
        // Called by the consumer, not under a queue monitor. Android posts
        // these to the UI; acknowledgement is delayed until that post runs.
        void blocked(Ticket ticket,Phase phase);
        void completed(Ticket ticket,Outcome outcome);
    }
    interface SampleObserver {void processed(boolean accepted);}
    static final class Ticket {
        final Capture capture;
        final Observer observer;
        private final InkPipeline owner;
        // Producer/consumer cancellation handshake. Others are worker-owned
        // except acknowledged (producer) and complete (release/acquire).
        private volatile boolean cancelled, complete;
        private boolean began, failed, discarded, acknowledged;
        private Ticket(InkPipeline owner,Capture capture,Observer observer) {
            this.owner=owner;this.capture=capture;this.observer=observer;
        }
    }
    private final InkLane lane;
    private final Engine engine;
    private final int maxStrokes;
    private Ticket open;
    private int outstanding,highWater;
    private long rejectedStrokes;
    // Guarded by this monitor. No native calls while held.
    private Ticket blocked;
    private Phase phase;
    private boolean resolving;
    InkPipeline(int packets,int strokes,Engine engine) {
        if(strokes<1) throw new IllegalArgumentException();
        this.lane=new InkLane(packets);this.engine=engine;maxStrokes=strokes;
    }
    void run() {lane.run();}
    synchronized Ticket begin(Capture capture,Observer observer) {
        if(open!=null || blocked!=null || outstanding>=maxStrokes) {rejectedStrokes++;return null;}
        Ticket t=new Ticket(this,capture,observer);
        if(!lane.offer(()->start(t),false)) {rejectedStrokes++;return null;}
        open=t;outstanding++;highWater=Math.max(highWater,outstanding);return t;
    }
    synchronized boolean sample(Ticket t,Sample sample,SampleObserver observer) {
        if(t==null || t!=open) return false;
        return lane.offer(()->{
            boolean accepted=t.began && !t.failed && !t.discarded && engine.sample(t.capture.id,sample);
            if(!accepted) t.failed=true;
            observer.processed(accepted);
        },false);
    }
    synchronized void finish(Ticket t,boolean cancel,boolean prefixOnly) {
        if(t==null || t!=open) throw new IllegalStateException("Invalid terminal ticket");
        t.cancelled=cancel;
        // One open gesture globally; nonterminal offers always reserve this slot.
        if(!lane.offer(()->seal(t,prefixOnly),true)) throw new IllegalStateException("Terminal reserve lost");
        open=null;
        // A failed DOWN can be cancelled while parked. The terminal still runs
        // in FIFO order, and all captured samples receive explicit outcomes.
        if(cancel && blocked==t && phase==Phase.BEGIN) resolve(t,true);
    }
    synchronized int outstanding() {return outstanding;}
    synchronized Ticket blockedTicket() {return blocked;}
    synchronized Phase blockedPhase(Ticket t) {return blocked==t?phase:null;}
    long[] counters() {return lane.counters();}
    synchronized long[] admissionCounters() {return new long[]{outstanding,highWater,rejectedStrokes,blocked!=null?1:0};}
    synchronized void acknowledge(Ticket t) {
        if(t==null || t.owner!=this || !t.complete || t.acknowledged) throw new IllegalStateException("Invalid acknowledgement");
        t.acknowledged=true;outstanding--;
    }
    private void start(Ticket t) {
        if(t.cancelled) {t.discarded=true;return;}
        t.began=engine.begin(t.capture);
        if(!t.began) block(t,Phase.BEGIN);
    }
    private void seal(Ticket t,boolean prefixOnly) {
        if(t.cancelled) {
            if(t.began) engine.cancel(t.capture.id);
            complete(t,Outcome.CANCELLED);
        } else if(t.discarded) complete(t,Outcome.DISCARDED);
        else if(!t.began) throw new IllegalStateException("Unresolved native admission");
        else if(prefixOnly || t.failed || !engine.end(t.capture.id)) block(t,Phase.END);
        else complete(t,Outcome.COMMITTED);
    }
    private void complete(Ticket t,Outcome outcome) {
        t.complete=true;t.observer.completed(t,outcome);
    }
    private void block(Ticket t,Phase p) {
        synchronized(this) {
            blocked=t;phase=p;lane.pause();
            // Close the race with producer cancellation before or during pause.
            if(p==Phase.BEGIN && t.cancelled) {t.discarded=true;unblock();return;}
        }
        t.observer.blocked(t,p);
    }
    private synchronized void unblock() {
        blocked=null;phase=null;resolving=false;lane.resume();
    }
    synchronized boolean resolve(Ticket t,boolean discard) {
        if(t==null || t!=blocked || resolving) return false;
        Phase p=phase;
        if(!lane.resolve(()->resolveOnWorker(t,p,discard))) return false;
        resolving=true;return true;
    }
    private void resolveOnWorker(Ticket t,Phase p,boolean discard) {
        if(p==Phase.BEGIN) {
            if(discard || t.cancelled) {t.discarded=true;unblock();return;}
            t.began=engine.begin(t.capture);
            if(t.began) {unblock();return;}
        } else {
            if(discard) {engine.cancel(t.capture.id);unblock();complete(t,Outcome.DISCARDED);return;}
            if(engine.end(t.capture.id)) {unblock();complete(t,Outcome.COMMITTED);return;}
        }
        // A failed retry exposes one new resolution opportunity. The worker
        // will take it only after returning from this control.
        synchronized(this) {
            if(p==Phase.BEGIN && t.cancelled) {t.discarded=true;unblock();return;}
            resolving=false;
        }
        t.observer.blocked(t,p);
    }
}
