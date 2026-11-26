package com.infinipaint.storage;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Binder;
import android.os.DeadObjectException;
import android.os.IBinder;
import android.os.Parcel;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.util.UUID;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.Test;
import static org.junit.Assert.*;

public final class ProcessRecoveryTest {
    @Test public void recoverOldOrCommittedGroupAtFourKillBoundaries() throws Exception {
        Context ctx=InstrumentationRegistry.getInstrumentation().getTargetContext();
        assertTrue(ctx.getPackageName().endsWith(".m03verify"));
        for(int phase=0;phase<4;phase++) runBoundary(ctx,phase);
    }
    private static void runBoundary(Context ctx,int phase) throws Exception {
        UUID fixture=UUID.randomUUID(),doc;
        File path=new File(ctx.getCacheDir(),"kill-test-"+fixture+".sqlite");
        try(DocumentDatabase db=DocumentDatabase.open(path,true)) {
            doc=db.createDocument();db.reserve(doc,2);
            db.commit(doc,0,1,1,1,w->w.insert(1,0,0,new byte[80],1,i->new byte[36]));
        }
        AtomicBoolean acknowledged=new AtomicBoolean();
        AtomicBoolean boundary=new AtomicBoolean();
        Binder ack=new Binder() {
            @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags) {
                if(code==FIRST_CALL_TRANSACTION+1) {
                    boundary.set(data.readInt()==phase);reply.writeNoException();return true;
                }
                if(code!=FIRST_CALL_TRANSACTION) return false;
                acknowledged.set(true);reply.writeNoException();return true;
            }
        };
        ArrayBlockingQueue<IBinder> bound=new ArrayBlockingQueue<>(1);
        ServiceConnection connection=new ServiceConnection() {
            public void onServiceConnected(ComponentName name,IBinder service){bound.offer(service);}
            public void onServiceDisconnected(ComponentName name){}
        };
        Intent cmd=new Intent().setClassName(ctx.getPackageName(),"com.infinipaint.storage.RecoveryProbeService");
        assertTrue(ctx.bindService(cmd,connection,Context.BIND_AUTO_CREATE));
        Parcel request=Parcel.obtain(),response=Parcel.obtain();
        try {
            IBinder service=bound.poll(15,TimeUnit.SECONDS);assertNotNull(service);
            request.writeString(fixture.toString());request.writeString(doc.toString());
            request.writeInt(phase);request.writeStrongBinder(ack);
            try {
                service.transact(IBinder.FIRST_CALL_TRANSACTION,request,response,0);
                response.readException();fail("Probe returned alive at boundary "+phase);
            } catch(DeadObjectException expected) { /* Only test child died */ }
        } finally {ctx.unbindService(connection);request.recycle();response.recycle();}
        assertEquals(phase==3,acknowledged.get());
        assertTrue("Did not reach requested boundary",boundary.get());
        long expectedId=phase<2?1:2;
        int expectedSamples=phase<2?1:2000;
        int[] bytes={0};int[] rows={0};
        try(DocumentDatabase db=DocumentDatabase.open(path,false)) {
            db.load(doc,1,2000L*36,new DocumentDatabase.Restore() {
                public void begin(UUID id,long revision,long ceiling,long count) {
                    assertEquals(doc,id);assertEquals(expectedId,revision);assertEquals(2,ceiling);assertEquals(1,count);
                }
                public void stroke(long id,long prev,long next,long content,long geometry,int n,byte[] header) {
                    assertEquals(expectedId,id);assertEquals(0,prev);assertEquals(0,next);assertEquals(expectedSamples,n);
                    assertEquals(expectedId,content);assertEquals(expectedId,geometry);rows[0]++;
                }
                public void chunk(byte[] b){bytes[0]+=b.length;}
                public void endStroke(){}
            });
            assertEquals(1,rows[0]);assertEquals(expectedSamples*36,bytes[0]);
        }
        // Keep failing fixtures intact. Remove only this successful test's files.
        for(String suffix:new String[]{"","-wal","-shm","-journal"}) {
            File f=new File(path.getPath()+suffix);
            if(f.exists()) assertTrue(f.delete());
        }
    }
}
