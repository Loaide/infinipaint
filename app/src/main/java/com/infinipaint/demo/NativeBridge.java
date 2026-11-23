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
 public static native boolean sample(int id,double x,double y,double seconds,float pressure,float tiltX,float tiltY);
 public static native boolean end(int id);
 public static native void cancel(int id);
 public static native void clear();
 public static native void surface(int width,int height);
 public static native void render(int mode);


}
