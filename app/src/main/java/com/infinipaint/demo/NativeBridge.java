package com.infinipaint.demo;
public final class NativeBridge {
 static { System.loadLibrary("ip_tablet"); }
 private NativeBridge() {}


 public static native void setCamera(
         double panX,
         double panY,
         double zoom
 );

 public static native void setSmoothingPreset(int preset);
 public static native boolean begin(int id,int toolType);
 static native boolean beginCaptured(int id,int toolType,double x,double y,double zoom,int preset);
 public static native boolean sample(int id,double x,double y,double seconds,float pressure,float tiltX,float tiltY);
 public static native boolean end(int id);
 public static native void cancel(int id);
 public static native void clear();
 public static native long revision();
 public static native long[] memory();
 public static native void suspend(boolean suspended);
 public static native int command(int operation,double[] worldPath,double tolerance);
 public static native void contextCreated();
 public static native void resizeViewport(int width,int height);
 public static native void render(int mode);
 static native boolean needsPublicationRetry();
 static native long[] plannerCounters();
 static native void configureSpatialCache(String path);
 static native long[] spatialCounters();
 static native long[] spatialLatency();
 static native long[] spanStreamCounters();
 static native long[] spatialCompactionCounters();
 static native long[] paintOrderCounters();
 static native long[] canonicalReplayCounters();


}
