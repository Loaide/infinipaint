package com.infinipaint.storage;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;

final class CanonicalJniHostTest {
    static void check(boolean b,String m) {if(!b) throw new AssertionError(m);}
    static void rejected(Runnable f) {
        try {f.run();}catch(IllegalStateException expected){return;}
        throw new AssertionError("Expected rejection");
    }
    static byte[] header() {
        ByteBuffer b=ByteBuffer.allocate(80).order(ByteOrder.LITTLE_ENDIAN);
        b.putInt(1).putInt(1).putInt(1).putInt(1).putInt(2).putFloat(1.9f);
        b.putDouble(1).putDouble(.007).putDouble(1);
        b.putFloat(.2f).putFloat(.3f).putFloat(.8f).putFloat(.5f).putDouble(4.25).putDouble(-8.5);
        check(b.position()==80,"header length");return b.array();
    }
    static byte[] samples(int n) {
        ByteBuffer b=ByteBuffer.allocate(n*36).order(ByteOrder.LITTLE_ENDIAN);
        for(int i=0;i<n;i++) b.putDouble(i/7.).putDouble((i%57)*.25).putDouble(1+i/5*.001)
                .putFloat((i%101)/100f).putFloat(0).putFloat(0);
        return b.array();
    }
    static byte[] read(byte[] data,int first,int count,int slice) {
        long t=StorePort.rangeBegin(header(),data.length/36,first,count);
        try {
            for(int p=0;p<data.length;p+=slice*36) StorePort.rangeChunk(t,data,p,Math.min(slice*36,data.length-p));
            return StorePort.rangeFinish(t);
        } finally {StorePort.rangeDiscard(t);}
    }
    public static void main(String[] args) {
        final int n=4097;byte[] raw=samples(n);
        byte[] a=read(raw,1800,128,64),b=read(raw,1800,128,17);
        check(Arrays.equals(a,b),"JNI chunk partition equivalence");
        ByteBuffer p=ByteBuffer.wrap(a).order(ByteOrder.LITTLE_ENDIAN);
        check(p.getInt(0)==0x31535043 && p.getInt(8)==164 && p.getInt(12)==1800 && p.getInt(16)==128 && p.getInt(24)==n,"wire header");
        int total=p.getInt(20);
        check(Arrays.equals(Arrays.copyOfRange(a,108+17*164,108+18*164),Arrays.copyOfRange(read(raw,1817,1,64),108,272)),"overlapping range bytes");
        byte[] terminal=read(raw,total-1,1,64);check(ByteBuffer.wrap(terminal).order(ByteOrder.LITTLE_ENDIAN).getInt(108)==3,"terminal kind");
        check(read(raw,total,1,64).length==108,"end sentinel");
        long busy=StorePort.rangeBegin(header(),n,0,128);
        rejected(()->StorePort.rangeBegin(header(),n,0,1));
        rejected(()->StorePort.rangeChunk(busy,raw,-1,36));
        rejected(()->StorePort.rangeChunk(busy,raw,0,65*36));
        StorePort.rangeChunk(busy,raw,0,64*36);
        rejected(()->StorePort.rangeFinish(busy));StorePort.rangeDiscard(busy);
        long next=StorePort.rangeBegin(header(),1,0,1);
        StorePort.rangeDiscard(busy);
        rejected(()->StorePort.rangeChunk(busy,raw,0,36));
        StorePort.rangeChunk(next,raw,0,36);check(StorePort.rangeFinish(next).length==272,"new token after stale discard");
        rejected(()->StorePort.rangeFinish(next));StorePort.rangeDiscard(next);
        byte[] bad=header();bad[0]=0;rejected(()->StorePort.rangeBegin(bad,1,0,1));
        for(int i=0;i<100;i++) {
            long t=StorePort.rangeBegin(header(),n,0,128);
            StorePort.rangeChunk(t,raw,0,36);StorePort.rangeDiscard(t);
        }
        check(Arrays.equals(a,read(raw,1800,128,64)),"repeat result after 100 cancelled leases");
        long[] counters=StorePort.rangeCounters();
        check(counters[0]==counters[1]&&counters[8]==0&&counters[9]<counters[10]&&counters[11]==64,"lease accounting");
        System.out.println("PASS real JNI range: partition parity, exact overlapping packet, terminal, stale/busy tokens, extent checks, partial finish, 100 discard cycles, zero active leases: "+Arrays.toString(counters));
    }
}
