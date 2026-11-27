package com.infinipaint.diag.matched;

import android.app.Activity;
import android.os.*;
import android.graphics.PixelFormat;
import android.view.*;
import java.io.*;

// Choreographer and graphics calls share a dedicated lane, never the UI looper.
public final class Bench extends Activity implements SurfaceHolder.Callback {
    static { System.loadLibrary("matched"); }
    private static native void start(Surface s,String api,String scene,String dir,int frames,int timer);
    private static native boolean step(long vsync);
    private static native void close();
    private HandlerThread lane; private Handler handler;
    private volatile boolean gone; private boolean launched;
    @Override public void onCreate(Bundle b){
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        lane=new HandlerThread("matched-render");lane.start();handler=new Handler(lane.getLooper());
        SurfaceView v=new SurfaceView(this);v.getHolder().setFormat(PixelFormat.RGBA_8888);
        v.getHolder().addCallback(this);setContentView(v);
    }
    public void surfaceCreated(SurfaceHolder h){}
    public void surfaceChanged(SurfaceHolder h,int format,int w,int height){
        if(launched || gone || w==0 || height==0)return;
        launched=true;
        final Surface s=h.getSurface();
        handler.post(()->{
            try{
                if(gone)throw new IOException("surface lost");
                start(s,getIntent().getStringExtra("api"),getIntent().getStringExtra("scene"),
                      getFilesDir().getAbsolutePath(),getIntent().getIntExtra("frames",1000),
                      getIntent().getIntExtra("timer",1));
                Choreographer c=Choreographer.getInstance();
                c.postFrameCallback(new Choreographer.FrameCallback(){
                    public void doFrame(long tick){
                        try{
                            if(gone)throw new IOException("surface lost");
                            if(step(tick))c.postFrameCallback(this);else done("ok");
                        }catch(Exception e){done("fail "+e);}
                    }
                });
            }catch(Exception e){done("fail "+e);}
        });
    }
    private void done(String result){
        close();
        try(FileOutputStream f=openFileOutput("status",MODE_PRIVATE)){
            f.write((getIntent().getIntExtra("run",0)+" "+result+"\n").getBytes("UTF-8"));
        }catch(IOException e){android.util.Log.e("IP_MATCH","status",e);}
        runOnUiThread(this::finish);
    }
    public void surfaceDestroyed(SurfaceHolder h){gone=true;}
    @Override public void onDestroy(){
        gone=true;handler.post(()->{close();lane.quitSafely();});super.onDestroy();
    }
}
