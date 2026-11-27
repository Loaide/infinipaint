package com.infinipaint.diag.matched;

import android.app.Activity;
import android.content.pm.ActivityInfo;
import android.graphics.PixelFormat;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Choreographer;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.concurrent.CountDownLatch;

// Diagnostic only. Replay a committed eight-chord suffix across a rotation
// round trip and a background/foreground surface rebuild in one process.
public final class Lifecycle extends Activity implements SurfaceHolder.Callback {
    private HandlerThread lane;
    private Handler handler;
    private SurfaceView view;
    private int stage=0;
    private volatile int generation=0;
    private boolean running, stopped, failed, waitingBackground, backgroundDestroyed;
    private volatile boolean opened;
    private File root;
    @Override public void onCreate(Bundle state){
        super.onCreate(state);
        root=new File(getFilesDir(),"life");
        if(!root.mkdir()){fail("life directory already exists or unavailable");return;}
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        lane=new HandlerThread("life-render");lane.start();handler=new Handler(lane.getLooper());
        attach();
    }
    private void attach(){
        view=new SurfaceView(this);view.getHolder().setFormat(PixelFormat.RGBA_8888);
        view.getHolder().addCallback(this);setContentView(view);
    }
    private void write(String name,String value) throws IOException{
        try(FileOutputStream f=new FileOutputStream(new File(root,name))){f.write((value+"\n").getBytes("UTF-8"));}
    }
    private void fail(String why){
        if(failed)return;failed=true;generation++;
        android.util.Log.e("IP_MATCH","lifecycle failed: "+why);
        try{write("status","fail "+why);}catch(IOException ignored){}
        finish();
    }
    @Override public void surfaceCreated(SurfaceHolder h){}
    @Override public void surfaceChanged(SurfaceHolder h,int format,int w,int height){
        if(failed||stopped||waitingBackground||running||stage>3||h!=view.getHolder()||w==0||height==0)return;
        if((stage==1)!=(w>height)||w==height)return;
        running=true;
        final int part=stage, token=++generation;
        final android.view.Surface surface=h.getSurface();
        final File dir=new File(root,"stage"+part);
        if(!dir.mkdir()){fail("stage directory");return;}
        handler.post(()->{
            try{
                if(token!=generation)return;
                opened=true; // also close partially initialized native resources on failure
                Bench.start(surface,"vki",part==0?"life-live":"life-pixel",dir.getAbsolutePath(),part==0?8:1,0);
                Choreographer c=Choreographer.getInstance();
                c.postFrameCallback(new Choreographer.FrameCallback(){
                    public void doFrame(long tick){
                        if(token!=generation)return;
                        try{
                            if(Bench.step(tick))c.postFrameCallback(this);
                            else{
                                // Teardown directly after the last submitted suffix frame;
                                // no readback or drain frames before the first teardown.
                                Bench.close();opened=false;
                                runOnUiThread(()->complete(part,token));
                            }
                        }catch(Exception e){runOnUiThread(()->fail(e.toString()));}
                    }
                });
            }catch(Exception e){runOnUiThread(()->fail(e.toString()));}
        });
    }
    private void complete(int part,int token){
        if(failed||token!=generation||part!=stage)return;
        running=false;
        try{write("stage"+part+".ok","ok");}catch(IOException e){fail(e.toString());return;}
        if(part==3){stage=4;try{write("status","ok");}catch(IOException e){fail(e.toString());return;}finish();return;}
        stage++;
        if(part==2){
            waitingBackground=true;
            // The host resumes only after onStop AND surfaceDestroyed have occurred.
            if(!moveTaskToBack(true))fail("moveTaskToBack failed");
        }else{
            setRequestedOrientation(part==0?ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE:ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);
            attach(); // fresh SurfaceView, old render resources already retired
        }
    }
    private void closeBarrier(){
        generation++;
        if(handler==null)return;
        CountDownLatch latch=new CountDownLatch(1);
        handler.post(()->{try{if(opened){Bench.close();opened=false;}}finally{latch.countDown();}});
        // SurfaceHolder requires rendering to stop before surfaceDestroyed returns.
        boolean interrupted=false;
        for(;;){try{latch.await();break;}catch(InterruptedException e){interrupted=true;}}
        if(interrupted)Thread.currentThread().interrupt();
    }
    @Override public void surfaceDestroyed(SurfaceHolder h){
        // A replaced view's destruction is expected; do not cancel its successor.
        if(view==null||h!=view.getHolder())return;
        closeBarrier();
        if(waitingBackground){backgroundDestroyed=true;markBackground();}
        else if(running&&!failed){running=false;fail("surface destroyed before stage completed");}
    }
    private void markBackground(){
        if(waitingBackground&&stopped&&backgroundDestroyed){
            try{write("background.ready","ok");}catch(IOException e){fail(e.toString());}
        }
    }
    @Override protected void onStop(){super.onStop();stopped=true;markBackground();}
    @Override protected void onStart(){
        super.onStart();stopped=false;
        if(waitingBackground&&backgroundDestroyed&&!failed){waitingBackground=false;attach();}
    }
    @Override protected void onDestroy(){
        closeBarrier();if(lane!=null)lane.quitSafely();super.onDestroy();
    }
}
