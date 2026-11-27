package com.infinipaint.demo;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Standalone host test; does not require Android or JUnit. */
final class InkLaneHostTest {
    static void check(boolean ok,String what) {if(!ok) throw new AssertionError(what);}
    static void waitFor(CountDownLatch l) throws InterruptedException {
        check(l.await(5,TimeUnit.SECONDS),"latch deadline");
    }
    public static void main(String[] args) throws Exception {
        InkLane lane=new InkLane(4);
        CountDownLatch parked=new CountDownLatch(1),release=new CountDownLatch(1),done=new CountDownLatch(1);
        List<Integer> seen=new ArrayList<>();
        AtomicReference<Throwable> failure=new AtomicReference<>();
        Thread worker=new Thread(lane::run);worker.setDaemon(true);
        worker.setUncaughtExceptionHandler((t,e)->failure.set(e));
        worker.start();
        try {
            check(lane.offer(()->{seen.add(0);parked.countDown();try {waitFor(release);}catch(InterruptedException e){throw new AssertionError(e);}},false),"admit DOWN");
            waitFor(parked);
            for(int i=1;i<=3;i++){final int sample=i;check(lane.offer(()->seen.add(sample),false),"admit sample");}
            check(!lane.offer(()->{throw new AssertionError("rejected sample executed");},false),"reserve terminal slot");
            check(lane.offer(()->{seen.add(4);done.countDown();},true),"admit terminal at saturation");
            check(!lane.offer(()->{},true),"bounded terminal queue");
            long[] peak=lane.counters();check(peak[0]==4 && peak[1]==4 && peak[2]==2,"queue counters");
            release.countDown();waitFor(done);
            check(seen.equals(List.of(0,1,2,3,4)),"FIFO exact once");
            // Reuse the ring across wraparound; distinguish two terminal kinds.
            for(int j=0;j<100;j++) {
                CountDownLatch ended=new CountDownLatch(1);
                check(lane.offer(()->{},false),"second DOWN");
                check(lane.offer(ended::countDown,true),"CANCEL");
                waitFor(ended);
            }
            check(lane.counters()[0]==0,"drained");
            check(failure.get()==null,"worker failed");
        } finally {release.countDown();worker.interrupt();worker.join(5000);}
        check(!worker.isAlive(),"interrupt idle consumer");
        System.out.println("PASS InkLane: parked consumer, FIFO, terminal reserve, bounded saturation, 100 reuse cycles, idle interrupt");
    }
}
