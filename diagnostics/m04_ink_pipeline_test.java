package com.infinipaint.demo;

import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Uses the production pipeline and queue, with a controllable native engine.
 * Each scenario checks exact outcomes; no sleeps as correctness conditions. */
final class InkPipelineHostTest {
    static void check(boolean ok,String what){if(!ok)throw new AssertionError(what);}
    static void await(CountDownLatch l){try{check(l.await(5,TimeUnit.SECONDS),"latch timeout");}catch(InterruptedException e){throw new AssertionError(e);}}
    static <T> T take(BlockingQueue<T> q){try{T t=q.poll(5,TimeUnit.SECONDS);check(t!=null,"receipt timeout");return t;}catch(InterruptedException e){throw new AssertionError(e);}}
    static final class Engine implements InkPipeline.Engine {
        final List<String> events=Collections.synchronizedList(new ArrayList<>());
        int active=-1;
        final AtomicInteger failBegin=new AtomicInteger(),failEnd=new AtomicInteger(),failSample=new AtomicInteger();
        volatile CountDownLatch entered,release;
        void parkOnce(){CountDownLatch e=entered,r=release;if(e!=null){entered=null;e.countDown();await(r);}}
        public boolean begin(InkPipeline.Capture c){
            check(active==-1,"second DOWN against unresolved native stroke");
            events.add("B"+c.x+":"+c.zoom);parkOnce();
            if(failBegin.getAndUpdate(x->Math.max(0,x-1))>0)return false;
            active=c.id;return true;
        }
        public boolean sample(int id,InkPipeline.Sample s){
            check(active==id,"sample on wrong stroke");events.add("S"+s.x);
            return failSample.getAndUpdate(x->Math.max(0,x-1))==0;
        }
        public boolean end(int id){check(active==id,"end on wrong stroke");events.add("E");
            if(failEnd.getAndUpdate(x->Math.max(0,x-1))>0)return false;
            active=-1;return true;
        }
        public void cancel(int id){check(active==id,"cancel on wrong stroke");events.add("C");active=-1;}
    }
    record Receipt(InkPipeline.Ticket ticket,InkPipeline.Phase phase,InkPipeline.Outcome outcome){}
    static final class Fixture implements AutoCloseable {
        final Engine engine=new Engine();
        final InkPipeline p;
        final BlockingQueue<Receipt> receipts=new LinkedBlockingQueue<>();
        final List<Boolean> accepted=Collections.synchronizedList(new ArrayList<>());
        final AtomicReference<Throwable> failure=new AtomicReference<>();
        final Thread worker;
        final InkPipeline.Observer observer=new InkPipeline.Observer(){
            public void blocked(InkPipeline.Ticket t,InkPipeline.Phase p){receipts.add(new Receipt(t,p,null));}
            public void completed(InkPipeline.Ticket t,InkPipeline.Outcome o){receipts.add(new Receipt(t,null,o));}
        };
        Fixture(int packets,int strokes){p=new InkPipeline(packets,strokes,engine);worker=new Thread(p::run);worker.setDaemon(true);
            worker.setUncaughtExceptionHandler((t,e)->failure.set(e));worker.start();}
        InkPipeline.Ticket begin(int x){InkPipeline.Ticket t=p.begin(new InkPipeline.Capture(7,2,x,0,4,1),observer);check(t!=null,"admit gesture "+x);return t;}
        boolean sample(InkPipeline.Ticket t,int x){return p.sample(t,new InkPipeline.Sample(x,2,x,1,0,0),accepted::add);}
        void stroke(int x){InkPipeline.Ticket t=begin(x);check(sample(t,x),"sample");p.finish(t,false,false);}
        Receipt receipt(){return take(receipts);}
        Receipt ack(InkPipeline.Outcome o){Receipt r=receipt();check(r.outcome==o && r.phase==null,"wrong terminal receipt "+r);p.acknowledge(r.ticket);return r;}
        void block(InkPipeline.Ticket t,InkPipeline.Phase phase){Receipt r=receipt();check(r.ticket==t&&r.phase==phase&&r.outcome==null,"wrong blocked head");check(p.blockedTicket()==t,"blocked state");}
        public void close(){if(engine.release!=null)engine.release.countDown();worker.interrupt();try{worker.join(5000);}catch(InterruptedException e){throw new AssertionError(e);}
            check(!worker.isAlive(),"worker shutdown");check(failure.get()==null,"worker exception "+failure.get());}
    }
    static void parkedMany(){try(Fixture f=new Fixture(256,32)){
        CountDownLatch e=new CountDownLatch(1);f.engine.entered=e;f.engine.release=new CountDownLatch(1);
        f.stroke(0);await(e);for(int i=1;i<32;i++)f.stroke(i);
        check(f.p.outstanding()==32,"32 outstanding gestures");check(f.p.begin(new InkPipeline.Capture(7,2,99,0,1,1),f.observer)==null,"bounded unacknowledged strokes");
        f.engine.release.countDown();for(int i=0;i<32;i++)f.ack(InkPipeline.Outcome.COMMITTED);
        List<String> expected=new ArrayList<>();for(int i=0;i<32;i++){expected.add("B"+(double)i+":4.0");expected.add("S"+(double)i);expected.add("E");}
        check(f.engine.events.equals(expected),"same-pointer gestures execute exact FIFO with capture camera");
        check(f.p.outstanding()==0 && f.p.admissionCounters()[1]==32 && f.p.admissionCounters()[2]==1,"admission counters");
    }}
    static void commitFailure(boolean discard){try(Fixture f=new Fixture(8,8)){
        CountDownLatch e=new CountDownLatch(1);f.engine.entered=e;f.engine.release=new CountDownLatch(1);f.engine.failEnd.set(2);
        InkPipeline.Ticket a=f.begin(0);check(f.sample(a,10),"prefix");f.p.finish(a,false,false);await(e);
        InkPipeline.Ticket b=f.begin(1);for(int j=0;j<4;j++)check(f.sample(b,20+j),"queued later sample");f.p.finish(b,false,false);
        f.engine.release.countDown();f.block(a,InkPipeline.Phase.END);
        check(f.engine.events.equals(List.of("B0.0:4.0","S10.0","E")),"later native DOWN did not pass failed seal");
        check(f.p.resolve(a,false),"retry admission");f.block(a,InkPipeline.Phase.END);
        check(f.p.resolve(a,discard),"second resolution admission");f.ack(discard?InkPipeline.Outcome.DISCARDED:InkPipeline.Outcome.COMMITTED);f.ack(InkPipeline.Outcome.COMMITTED);
        check(f.accepted.equals(List.of(true,true,true,true,true)),"later queued samples retained");check(f.p.outstanding()==0,"all acks");
    }}
    static void admissionFailure(boolean discard){try(Fixture f=new Fixture(12,4)){
        CountDownLatch e=new CountDownLatch(1);f.engine.entered=e;f.engine.release=new CountDownLatch(1);f.engine.failBegin.set(2);
        InkPipeline.Ticket a=f.begin(0);await(e);check(f.sample(a,10),"queued sample");f.p.finish(a,false,false);f.stroke(1);
        f.engine.release.countDown();f.block(a,InkPipeline.Phase.BEGIN);
        check(f.engine.events.equals(List.of("B0.0:4.0")),"failed DOWN parked entire queue");
        check(f.p.resolve(a,false),"retry DOWN");f.block(a,InkPipeline.Phase.BEGIN);
        check(f.p.resolve(a,discard),"resolve DOWN");f.ack(discard?InkPipeline.Outcome.DISCARDED:InkPipeline.Outcome.COMMITTED);f.ack(InkPipeline.Outcome.COMMITTED);
        check(f.accepted.equals(List.of(!discard,true)),"discard explicitly accounts for unprocessed sample");
    }}
    static void fullAndPrefix(){try(Fixture f=new Fixture(4,4)){
        CountDownLatch e=new CountDownLatch(1);f.engine.entered=e;f.engine.release=new CountDownLatch(1);
        InkPipeline.Ticket a=f.begin(0);await(e);check(f.sample(a,1)&&f.sample(a,2)&&f.sample(a,3),"fill queue");
        check(!f.sample(a,4),"reject overflow");f.p.finish(a,false,true);check(f.p.counters()[0]==4,"terminal slot reserved");
        f.engine.release.countDown();f.block(a,InkPipeline.Phase.END);check(f.engine.events.equals(List.of("B0.0:4.0","S1.0","S2.0","S3.0")),"no automatic truncated commit");
        check(f.p.resolve(a,false),"retry prefix despite full queue");f.ack(InkPipeline.Outcome.COMMITTED);
    }}
    static void sampleFailure(){try(Fixture f=new Fixture(12,4)){
        f.engine.failSample.set(1);InkPipeline.Ticket a=f.begin(0);check(f.sample(a,1)&&f.sample(a,2),"enqueue failed prefix");f.p.finish(a,false,false);
        f.block(a,InkPipeline.Phase.END);check(f.engine.events.equals(List.of("B0.0:4.0","S1.0")),"failed native sample freezes later suffix");
        check(f.accepted.equals(List.of(false,false)),"rejected samples explicitly reported");check(f.p.resolve(a,true),"discard failed prefix");f.ack(InkPipeline.Outcome.DISCARDED);
    }}
    static void cancelRacingRetry(){try(Fixture f=new Fixture(8,4)){
        f.engine.failBegin.set(2);InkPipeline.Ticket a=f.begin(0);f.block(a,InkPipeline.Phase.BEGIN);
        CountDownLatch e=new CountDownLatch(1);f.engine.entered=e;f.engine.release=new CountDownLatch(1);
        check(f.p.resolve(a,false),"retry begin");await(e);check(f.sample(a,3),"sample while retrying");f.p.finish(a,true,false);
        f.engine.release.countDown();f.ack(InkPipeline.Outcome.CANCELLED);check(f.p.blockedTicket()==null,"cancel raced failed retry without deadlock");
        f.stroke(2);f.ack(InkPipeline.Outcome.COMMITTED);
    }}
    static void cancelParked(){try(Fixture f=new Fixture(8,4)){
        f.engine.failBegin.set(1);InkPipeline.Ticket a=f.begin(0);f.block(a,InkPipeline.Phase.BEGIN);f.p.finish(a,true,false);
        f.ack(InkPipeline.Outcome.CANCELLED);f.stroke(2);f.ack(InkPipeline.Outcome.COMMITTED);
    }}
    static void ackBoundAndStress(){try(Fixture f=new Fixture(256,32)){
        // Native completion is not producer acknowledgement. The UI backlog
        // remains bounded even when the worker is faster than the UI.
        for(int i=0;i<32;i++)f.stroke(i);
        List<Receipt> pending=new ArrayList<>();for(int i=0;i<32;i++)pending.add(f.receipt());
        check(f.p.outstanding()==32 && f.p.begin(new InkPipeline.Capture(7,2,0,0,1,1),f.observer)==null,"unacked callback bound");
        for(Receipt r:pending)f.p.acknowledge(r.ticket);
        boolean rejected=false;try{f.p.acknowledge(pending.get(0).ticket);}catch(IllegalStateException e){rejected=true;}check(rejected,"duplicate ack rejected");
        Random rng=new Random(13792);int total=0;
        for(int batch=0;batch<500;batch++){
            int n=1+rng.nextInt(20);List<InkPipeline.Outcome> outcomes=new ArrayList<>();
            for(int j=0;j<n;j++){
                InkPipeline.Ticket t=f.begin(total++);check(f.sample(t,total),"stress sample");boolean cancel=rng.nextInt(5)==0;f.p.finish(t,cancel,false);
                outcomes.add(cancel?InkPipeline.Outcome.CANCELLED:InkPipeline.Outcome.COMMITTED);
            }
            for(InkPipeline.Outcome o:outcomes)f.ack(o);
            check(f.p.outstanding()==0 && f.p.blockedTicket()==null,"stress batch fully drained");
        }
        check(total>5000,"stress coverage");System.out.println("PASS deterministic mixed gesture stress: "+total+" strokes");
    }}
    public static void main(String[] args){parkedMany();commitFailure(false);commitFailure(true);admissionFailure(false);admissionFailure(true);fullAndPrefix();sampleFailure();cancelRacingRetry();cancelParked();ackBoundAndStress();
        System.out.println("PASS production InkPipeline: FIFO, both blocked phases, repeated retry/discard, reserved terminal, sample failure, cancel races, bounded callbacks");}
}
