package com.infinipaint.storage;

import android.app.ActivityManager;
import android.content.Context;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import com.infinipaint.demo.NativeBridge;
import java.io.File;
import java.util.HashMap;
import java.util.UUID;

/** Process-scoped owner. UI signals work; only the HandlerThread opens SQLite or streams samples. */
public final class EditorStore {
    public interface Listener {void changed(); void restore(double x,double y,double zoom);}
    private static EditorStore instance;
    public static synchronized EditorStore get(Context context) {
        if(instance==null) instance=new EditorStore(context.getApplicationContext());
        return instance;
    }
    private final Handler main=new Handler(Looper.getMainLooper()), io;
    private final File path;
    private final long resident;
    // Main thread
    private Listener listener;
    private boolean busy;
    private String message="Opening catalog";
    private double x,y,zoom=1;
    private long sequence;
    private UUID active;
    private boolean navDirty;
    // Shared flags protecting the number of pending callbacks, not the data path
    private boolean drainPosted,signal;
    // Worker only
    private DocumentDatabase db;
    private UUID current,token;
    private long tokenBase=-1;
    private int failures;
    private volatile long durable;
    private EditorStore(Context context) {
        path=new File(context.getNoBackupFilesDir(),"documents-v1.sqlite");
        ActivityManager.MemoryInfo mem=new ActivityManager.MemoryInfo();
        ((ActivityManager)context.getSystemService(Context.ACTIVITY_SERVICE)).getMemoryInfo(mem);
        resident=Math.min(384L<<20,mem.totalMem/16); // physical RAM, not swap
        HandlerThread thread=new HandlerThread("IP-document-store");
        thread.start();io=new Handler(thread.getLooper());
    }
    public void attach(Listener l) {
        listener=l;
        if(active==null && !busy) open(false,false);
        else if(active!=null) l.restore(x,y,zoom);
    }
    public void detach(Listener l) {if(listener==l) listener=null;}
    public String status() {
        return message+(active==null?"":" | "+active.toString().substring(0,8)+" "+durable+"/"+NativeBridge.revision());
    }
    private void show(String msg) {message=msg;if(listener!=null) listener.changed();}
    public void open(boolean create,boolean next) {
        if(busy) return;
        busy=true;
        NativeBridge.suspend(true);
        flushView();
        show("Opening");
        io.post(()->{
            try {
                if(db==null) db=DocumentDatabase.open(path,!path.exists());
                drain();
                UUID id=create?db.createDocument():next?db.next(current):db.selected();
                if(id==null) id=db.next(null);
                if(id==null) id=db.createDocument();
                final UUID target=id;
                DocumentDatabase.View view=db.view(target);
                if(!target.equals(current)) {
                    final long[] rev={0};
                    db.load(target,resident/512,resident/8,new DocumentDatabase.Restore() {
                        public void begin(UUID doc,long r,long ceiling,long count) {
                            rev[0]=r;
                            StorePort.begin(doc.getMostSignificantBits(),doc.getLeastSignificantBits(),r,ceiling,count);
                        }
                        public void stroke(long id,long prev,long next,long cr,long gr,int n,byte[] h) {
                            StorePort.stroke(id,prev,cr,gr,n,h);
                        }
                        public void chunk(byte[] b) {StorePort.chunk(b);}
                        public void endStroke() {StorePort.endStroke();}
                    });
                    long first=db.reserve(target,65536);
                    StorePort.activate(first,first+65535,resident/3);
                    current=target;durable=rev[0];
                }
                db.select(target);
                main.post(()->{
                    active=target; x=view.x;y=view.y;zoom=view.zoom;sequence=view.seq;busy=false;
                    if(listener!=null) listener.restore(x,y,zoom);
                    NativeBridge.suspend(false);show("Saved");
                });
            } catch(RuntimeException e) {
                main.post(()->{busy=false;NativeBridge.suspend(false);show("Open failed: "+e.getMessage());});
            } finally {StorePort.discard();}
        });
    }
    private void drain() {
        if(current==null) return;
        long[] g;
        while((g=StorePort.delta(current.getMostSignificantBits(),current.getLeastSignificantBits()))!=null) {
            if(tokenBase!=g[0]) {tokenBase=g[0];token=UUID.randomUUID();}
            final int count=Math.toIntExact(g[4]);
            db.commit(current,token,g[0],g[1],g[2],g[3],w->{
                HashMap<Long,long[]> inserts=new HashMap<>();
                for(int i=0;i<count;i++) {
                    long[] row=StorePort.row(i);
                    if(row[3]==0) continue;
                    w.removeIfPresent(row[0]);
                    if(row[4]!=0) {
                        row[3]=i; inserts.put(row[0],row);
                    }
                }
                int written=0;
                for(long[] first:inserts.values()) {
                    if(inserts.containsKey(first[1])) continue;
                    for(long[] r=first;r!=null;r=inserts.get(r[2])) {
                        if(++written>inserts.size()) throw new IllegalStateException("Cyclic changed order");
                        int index=Math.toIntExact(r[3]);
                        w.insertAfter(r[0],r[1],StorePort.header(index),Math.toIntExact(r[4]),i->StorePort.data(index,i));
                    }
                }
                if(written!=inserts.size()) throw new IllegalStateException("Incomplete order");
            });
            StorePort.ack();
            durable=g[0]+1; tokenBase=-1; token=null;
        }
    }
    public void command(int op,double[] path,double tolerance) {
        if(busy || active==null) return;
        busy=true;NativeBridge.suspend(true);show("Editing");
        io.post(()->{
            int result=NativeBridge.command(op,path,tolerance);
            main.post(()->{
                busy=false;NativeBridge.suspend(false);
                String[] errors={"Edited","Edit conflict","Invalid edit","ID limit","Undo budget exceeded",
                        "Not enough memory","Nothing to undo/redo","Save queue full"};
                show(result>=0 && result<errors.length?errors[result]:"Edit failed");
                if(result==0) save();
            });
        });
    }
    public synchronized void save() {
        if(active==null && !busy) {open(false,false);return;}
        signal=true;
        if(drainPosted) return;
        drainPosted=true;
        io.post(this::runSave);
    }
    private void runSave() {
        synchronized(this) {signal=false;}
        try {
            drain(); failures=0;
            main.post(()->{if(!busy) show("Saved");});
        } catch(RuntimeException e) {
            main.post(()->show("Save failed; retry Save: "+e.getMessage()));
            if(++failures<=3) {io.postDelayed(this::runSave,1000L<<failures);return;}
        }
        synchronized(this) {
            if(signal) io.post(this::runSave); else drainPosted=false;
        }
    }
    public void camera(double px,double py,double z) {
        if(active==null || busy || (x==px && y==py && zoom==z)) return;
        x=px;y=py;zoom=z;
        if(!navDirty) {navDirty=true;main.postDelayed(viewTask,750);}
    }
    private final Runnable viewTask=this::flushView;
    public void flushView() {
        main.removeCallbacks(viewTask);
        if(!navDirty || active==null) return;
        navDirty=false;
        UUID doc=active;long seq=++sequence;double px=x,py=y,z=zoom;
        io.post(()->{
            try {db.saveView(doc,seq,px,py,z);}
            catch(RuntimeException e) {
                main.post(()->{if(doc.equals(active) && seq==sequence) navDirty=true;show("View save failed: "+e.getMessage());});
            }
        });
    }
}
