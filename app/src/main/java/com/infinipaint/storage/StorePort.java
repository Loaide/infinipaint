package com.infinipaint.storage;

/** Private JNI port; all streaming and candidate construction runs on the storage worker. */
final class StorePort {
    static {System.loadLibrary("ip_tablet");}
    private StorePort() {}
    static native void begin(long hi,long lo,long revision,long ceiling,long count,long physicalSlice);
    static native void stroke(long id,long prev,long content,long geometry,int samples,byte[] header);
    static native void chunk(byte[] data);
    static native void endStroke();
    static native void stage(long first,long last,long meshBudget);
    static native void activate(double x,double y,double zoom);
    static native void discard();
    static native boolean needsIds();
    static native boolean refill(long first,long last);
    static native long[] delta(long hi,long lo);
    static native long[] row(int index);
    static native byte[] header(int index);
    static native byte[] data(int index,int ordinal);
    static native void ack();
}
