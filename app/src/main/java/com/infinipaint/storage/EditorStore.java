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
    public interface Listener {void changed(); void redraw(); void restore(double x,double y,double zoom);}
    private static EditorStore instance;
    public static synchronized EditorStore get(Context context) {
        if(instance==null) instance=new EditorStore(context.getApplicationContext());
        return instance;
    }
    private final Handler main=new Handler(Looper.getMainLooper()), io;
    private final File path;
    private final android.content.SharedPreferences catalogState;
    private final long resident;
    // Main thread
    private Listener listener;
    private boolean busy, opening;
    private String message="Opening catalog";
    private double x,y,zoom=1;
    private long sequence;
    private UUID active;
    private UUID barrierDoc;
    private long barrierEdit,barrierView,viewDurable;
    private boolean navDirty;
    private static final class ViewWrite {
        final UUID doc;
        final long seq;
        final double x,y,z;
        ViewWrite(UUID doc,long seq,double x,double y,double z) {
            this.doc=doc;this.seq=seq;this.x=x;this.y=y;this.z=z;
        }
    }
    private ViewWrite latestView;
    private boolean viewPosted;
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
        catalogState=context.getSharedPreferences("document-catalog-state",Context.MODE_PRIVATE);
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
    public void redrawCurrent() {if(listener!=null) listener.redraw();}
    public boolean acceptsInput() {return !busy && active!=null;}
    /** Keep navigation on the active document during edits, not during a switch. */
    public boolean acceptsNavigation() {return !opening && active!=null;}
    public String status() {
        long visible=NativeBridge.revision(),stored=durable;
        String label=message.equals("Saved") && visible!=stored?"Saving":message;
        return label+(active==null?"":" | "+active.toString().substring(0,8)+" "+stored+"/"+visible);
    }
    private void show(String msg) {message=msg;if(listener!=null) listener.changed();}
    /** Nonblocking user barrier. One newest target subsumes older same-session
     * barriers; do not announce completion for a merely visible or queued edit. */
    public void flush() {
        if(!acceptsInput()) {if(!busy && active==null) open(false,false);return;}
        flushView();barrierDoc=active;barrierEdit=NativeBridge.revision();barrierView=sequence;
        show("Flushing revision "+barrierEdit);
        save();
    }
    public void open(boolean create,boolean next) {
        if(busy) return;
        busy=true;opening=true;
        NativeBridge.suspend(true);
        flushView();
        show("Opening");
        io.post(()->{
            try {
                if(db==null) {
                    boolean known=catalogState.getBoolean("initialized",false);
                    db=DocumentDatabase.open(path,!known && !path.exists());
                    if(!known && !catalogState.edit().putBoolean("initialized",true).commit()) {
                        db.close();db=null;throw new IllegalStateException("Cannot retain catalog initialization marker");
                    }
                }
                writeLatestView(); drain();
                UUID id=create?db.createDocument():next?db.next(current):db.selected();
                if(id==null) id=db.next(null);
                if(id==null) id=db.createDocument();
                final UUID target=id;
                DocumentDatabase.View view=db.view(target);
                if(!target.equals(current)) {
                    durable=activate(db,target,resident,view);current=target;
                }
                main.post(()->{
                    active=target; x=view.x;y=view.y;zoom=view.zoom;sequence=view.seq;viewDurable=view.seq;
                    barrierDoc=null;busy=false;opening=false;
                    if(listener!=null) listener.restore(x,y,zoom);
                    NativeBridge.suspend(false);show("Saved");
                });
            } catch(RuntimeException e) {
                main.post(()->{busy=false;opening=false;NativeBridge.suspend(false);show("Open failed: "+e.getMessage());});
            }
        });
    }
    static long activate(DocumentDatabase db,UUID target,long resident,DocumentDatabase.View view) {
        final long[] rev={0};
        try {
            db.load(target,resident/512,resident/8,new DocumentDatabase.Restore() {
                public void begin(UUID doc,long r,long ceiling,long count) {
                    rev[0]=r;StorePort.begin(doc.getMostSignificantBits(),doc.getLeastSignificantBits(),r,ceiling,count,resident);
                }
                public void stroke(long id,long prev,long next,long cr,long gr,int n,byte[] h) {
                    StorePort.stroke(id,prev,cr,gr,n,h);
                }
                public void chunk(byte[] b) {StorePort.chunk(b);}
                public void endStroke() {StorePort.endStroke();}
            });
            long first=db.reserve(target,65536);
            StorePort.stage(first,first+65535,resident/3);db.select(target);
            StorePort.activate(view.x,view.y,view.zoom);
            return rev[0];
        } finally {StorePort.discard();}
    }
    private void drain() {
        if(current==null) return;
        long[] g;
        while((g=StorePort.delta(current.getMostSignificantBits(),current.getLeastSignificantBits()))!=null) {
            if(tokenBase!=g[0]) {tokenBase=g[0];token=UUID.randomUUID();}
            persist(db,current,token,g);
            StorePort.ack();durable=g[0]+1;tokenBase=-1;token=null;
        }
    }
    static void persist(DocumentDatabase db,UUID current,UUID token,long[] g) {
        final int count=Math.toIntExact(g[4]);
        db.commit(current,token,g[0],g[1],g[2],g[3],w->{
                if(g[3]==0) {w.clear();return;}
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
                        w.restoreGeometryRevision(r[0],r[5]);
                    }
                }
                if(written!=inserts.size()) throw new IllegalStateException("Incomplete order");
        });
    }
    public void command(int op,double[] path,double tolerance) {
        if(busy || active==null) return;
        busy=true;NativeBridge.suspend(true);show("Editing");
        io.post(()->{
            long previous=NativeBridge.revision();
            int result=NativeBridge.command(op,path,tolerance);
            main.post(()->{
                busy=false;NativeBridge.suspend(false);
                String[] errors={"Edited","Edit conflict","Invalid edit","ID limit","Undo budget exceeded",
                        "Not enough memory","Nothing to undo/redo","Save queue full"};
                show(result>=0 && result<errors.length?errors[result]:"Edit failed");
                if(result==0 && NativeBridge.revision()!=previous) {
                    if(listener!=null) listener.redraw();
                    save();
                }
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
            writeLatestView();drain();
            if(current!=null && StorePort.needsIds()) {
                long first=db.reserve(current,65536);
                if(!StorePort.refill(first,first+65535)) throw new IllegalStateException("ID block not installed");
            }
            failures=0;
            UUID saved=current;long confirmed=durable;
            main.post(()->{
                if(busy || saved==null || !saved.equals(active)) return;
                if(saved.equals(barrierDoc)) {
                    if(confirmed<barrierEdit || viewDurable<barrierView) return;
                    barrierDoc=null;
                    show("Flushed revision "+barrierEdit);
                } else show("Saved");
            });
        } catch(RuntimeException e) {
            main.post(()->show("Save failed; retry Save: "+e.getMessage()));
            if(++failures<=3) {io.postDelayed(this::runSave,1000L<<failures);return;}
        }
        synchronized(this) {
            if(signal) io.post(this::runSave); else drainPosted=false;
        }
    }
    public void camera(double px,double py,double z) {
        if(!acceptsNavigation() || (x==px && y==py && zoom==z)) return;
        x=px;y=py;zoom=z;
        if(!navDirty) {navDirty=true;main.postDelayed(viewTask,750);}
    }
    private final Runnable viewTask=this::flushView;
    public synchronized void flushView() {
        main.removeCallbacks(viewTask);
        if(!navDirty || active==null) return;
        navDirty=false;
        latestView=new ViewWrite(active,++sequence,x,y,zoom);
        if(viewPosted) return;
        viewPosted=true;
        io.post(()->{
            synchronized(this) {viewPosted=false;}
            try {writeLatestView();}
            catch(RuntimeException e) {main.post(()->show("View save failed: "+e.getMessage()));}
        });
    }
    private void writeLatestView() {
        ViewWrite v;
        synchronized(this) {v=latestView;}
        if(v==null) return;
        if(!db.saveView(v.doc,v.seq,v.x,v.y,v.z) && db.view(v.doc).seq<v.seq)
            throw new IllegalStateException("Camera save not acknowledged");
        synchronized(this) {if(latestView==v) latestView=null;}
        main.post(()->{if(v.doc.equals(active)) viewDurable=Math.max(viewDurable,v.seq);});
    }
}
