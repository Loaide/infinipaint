package com.infinipaint.storage;

import android.app.Application;
import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import java.io.File;
import java.util.UUID;

/**
 * Verification APK only. The instrumentation drives this private process through
 * Binder. It cannot open the application's catalog or kill the instrumentation.
 */
public final class RecoveryProbeService extends Service {
    private final Binder control=new Binder() {
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply,int flags)
                throws android.os.RemoteException {
            if(code!=FIRST_CALL_TRANSACTION) return super.onTransact(code,data,reply,flags);
            if(!getPackageName().endsWith(".m03verify") ||
                    !Application.getProcessName().equals(getPackageName()+":recovery_probe") ||
                    Binder.getCallingUid()!=Process.myUid()) throw new SecurityException("Not private test process");
            UUID fixture=UUID.fromString(data.readString());
            UUID doc=UUID.fromString(data.readString());
            int phase=data.readInt();
            if(phase<0 || phase>3) throw new IllegalArgumentException("phase");
            IBinder ack=data.readStrongBinder();
            File catalog=new File(getCacheDir(),"kill-test-"+fixture+".sqlite");
            try(DocumentDatabase db=DocumentDatabase.open(catalog,false)) {
                if(phase==0) terminate(ack,phase);
                db.commit(doc,1,2,2,1,w->{
                    w.remove(1);
                    w.insert(2,0,0,new byte[80],2000,i->{
                        if(phase==1 && i==1) terminate(ack,phase);
                        return new byte[i==0?DocumentDatabase.CHUNK_BYTES:2000*36-DocumentDatabase.CHUNK_BYTES];
                    });
                });
                if(phase==3) {
                    Parcel notice=Parcel.obtain(),received=Parcel.obtain();
                    try {ack.transact(FIRST_CALL_TRANSACTION,notice,received,0);received.readException();}
                    finally {notice.recycle();received.recycle();}
                }
                terminate(ack,phase);
            }
            return true;
        }
    };
    private static void terminate(IBinder ack,int phase) {
        Parcel p=Parcel.obtain(),r=Parcel.obtain();
        try {
            p.writeInt(phase);
            if(!ack.transact(IBinder.FIRST_CALL_TRANSACTION+1,p,r,0)) throw new IllegalStateException("No phase witness");
            r.readException();
        } catch(android.os.RemoteException e) {throw new IllegalStateException(e);}
        finally {p.recycle();r.recycle();}
        Process.killProcess(Process.myPid());
        throw new AssertionError("test process did not terminate");
    }
    @Override public IBinder onBind(Intent intent) {return control;}
}
