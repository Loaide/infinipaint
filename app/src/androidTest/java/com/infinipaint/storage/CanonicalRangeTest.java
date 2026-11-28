package com.infinipaint.storage;

import static org.junit.Assert.*;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteException;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import java.io.File;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.UUID;
import java.util.concurrent.CancellationException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** Real Android SQLite + production native decoder. Scratch catalogs only. */
@RunWith(AndroidJUnit4.class)
public final class CanonicalRangeTest {
    private static final int N=4097, PACKET=164, HEADER=108;
    private Path dir;
    private File file;
    @Before public void setup() throws Exception {
        dir=Files.createTempDirectory(InstrumentationRegistry.getInstrumentation().getTargetContext().getCacheDir().toPath(),"canonical-range-");
        file=dir.resolve("test.sqlite").toFile();
    }
    @After public void cleanup() throws Exception {
        if(dir!=null) {try(var files=Files.newDirectoryStream(dir)) {for(Path f:files)Files.delete(f);}Files.delete(dir);}
    }
    private static byte[] header() {
        ByteBuffer b=ByteBuffer.allocate(80).order(ByteOrder.LITTLE_ENDIAN);
        b.putInt(1).putInt(1).putInt(1).putInt(1).putInt(2).putFloat(1.9f);
        b.putDouble(1).putDouble(.007).putDouble(1);
        b.putFloat(.2f).putFloat(.3f).putFloat(.8f).putFloat(.5f).putDouble(4.25).putDouble(-8.5);
        return b.array();
    }
    private static byte[] raw(int n,double offset) {
        ByteBuffer b=ByteBuffer.allocate(n*36).order(ByteOrder.LITTLE_ENDIAN);
        for(int i=0;i<n;i++) b.putDouble(offset+i/7.).putDouble((i%57)*.25).putDouble(1+i/5*.001)
                .putFloat((i%101)/100f).putFloat(0).putFloat(0);
        return b.array();
    }
    private static byte[] chunk(byte[] b,int i) {
        return Arrays.copyOfRange(b,i*DocumentDatabase.CHUNK_BYTES,Math.min(b.length,(i+1)*DocumentDatabase.CHUNK_BYTES));
    }
    private static void insert(DocumentDatabase.Writer w,long id,long prev,byte[] b) {
        w.insertAfter(id,prev,header(),b.length/36,i->chunk(b,i));
    }
    private static UUID fixture(DocumentDatabase db,double offset) {
        UUID id=db.createDocument();assertEquals(1L,db.reserve(id,16));
        db.commit(id,0,1,2,2,w->{insert(w,1,0,raw(N,offset));insert(w,2,1,raw(5,offset+10));});
        return id;
    }
    private static DocumentDatabase.CurveRange read(DocumentDatabase db,UUID id,long revision,long stroke,int first,int count) {
        return db.readCurveRange(id,revision,stroke,1,1,first,count,N*36,()->false);
    }
    private static byte[] bytes(DocumentDatabase.CurveRange r) {
        ByteBuffer p=r.packet();assertTrue(p.isReadOnly());byte[] out=new byte[p.remaining()];p.get(out);return out;
    }
    private static void noLease() {assertEquals(0L,DocumentDatabase.canonicalRangeCounters()[8]);}
    @Test public void exactOverlappingRangesRevisionAndNamespace() {
        UUID saved;byte[] expected;
        try(DocumentDatabase db=DocumentDatabase.open(file,true)) {
            UUID first=fixture(db,0),second=fixture(db,10000);db.select(second);saved=first;
            DocumentDatabase.CurveRange a=read(db,first,1,1,1800,128);
            assertEquals(first,a.doc);assertEquals(1L,a.revision);assertEquals(1L,a.id);
            assertEquals(1L,a.content);assertEquals(1L,a.geometry);
            ByteBuffer p=a.packet();assertEquals(0x31535043,p.getInt(0));assertEquals(PACKET,p.getInt(8));
            assertEquals(1800,p.getInt(12));assertEquals(128,p.getInt(16));assertEquals(N,p.getInt(24));
            assertEquals(HEADER+128*PACKET,p.remaining());
            int total=p.getInt(20);assertTrue(total>1928);
            assertArrayEquals(Arrays.copyOfRange(bytes(a),HEADER+17*PACKET,HEADER+18*PACKET),
                    Arrays.copyOfRange(bytes(read(db,first,1,1,1817,1)),HEADER,HEADER+PACKET));
            assertEquals(3,read(db,first,1,1,total-1,1).packet().getInt(HEADER));
            assertEquals(HEADER,read(db,first,1,1,total,1).packet().remaining());
            assertFalse(Arrays.equals(bytes(a),bytes(read(db,second,1,1,1800,128))));
            assertEquals(second,db.selected()); // read does not change selection
            db.commit(first,1,1,3,3,w->insert(w,3,2,raw(2,50)));
            assertThrows(SQLiteException.class,()->read(db,first,1,1,1800,128));
            assertThrows(SQLiteException.class,()->db.readCurveRange(first,2,1,2,2,0,1,N*36,()->false));
            assertArrayEquals(bytes(a),bytes(read(db,first,2,1,1800,128)));
            assertThrows(IllegalStateException.class,()->db.commit(first,2,2,3,2,w->{w.remove(1);throw new IllegalStateException("interrupted group");}));
            assertArrayEquals(bytes(a),bytes(read(db,first,2,1,1800,128)));expected=bytes(a);noLease();
        }
        try(DocumentDatabase db=DocumentDatabase.open(file,false)) {
            assertArrayEquals(expected,bytes(read(db,saved,2,1,1800,128)));noLease();
        }
    }
    @Test public void cancelledAndOversizedRequestsReleaseNativeLease() throws Exception {
        try(DocumentDatabase db=DocumentDatabase.open(file,true)) {
            UUID id=fixture(db,0);AtomicInteger polls=new AtomicInteger();
            assertThrows(CancellationException.class,()->db.readCurveRange(id,1,1,1,1,0,128,N*36,()->polls.incrementAndGet()>12));
            noLease();assertTrue(polls.get()>12);
            assertThrows(SQLiteException.class,()->db.readCurveRange(id,1,1,1,1,0,128,N*36-1,()->false));
            assertThrows(IllegalArgumentException.class,()->read(db,id,1,1,0,129));
            noLease();
            AtomicReference<Throwable> other=new AtomicReference<>();
            Thread thread=new Thread(()->{try{read(db,id,1,1,0,1);}catch(Throwable e){other.set(e);}});
            thread.start();thread.join();assertTrue(other.get() instanceof IllegalStateException);
            assertEquals(HEADER+128*PACKET,read(db,id,1,1,0,128).packet().remaining());noLease();
        }
    }
    @Test public void lateCorruptionRejectsEvenFirstSpanRequest() {
        UUID broken,healthy;
        try(DocumentDatabase db=DocumentDatabase.open(file,true)) {
            broken=fixture(db,0);healthy=fixture(db,100);db.select(healthy);
            assertEquals(HEADER+PACKET,read(db,broken,1,1,0,1).packet().remaining());
        }
        int ordinal=(N-1)/(DocumentDatabase.CHUNK_BYTES/36);
        byte[] last=chunk(raw(N,0),ordinal);
        ByteBuffer.wrap(last).order(ByteOrder.LITTLE_ENDIAN).putFloat(last.length-12,.123f);
        try(SQLiteDatabase db=SQLiteDatabase.openDatabase(file.getPath(),null,SQLiteDatabase.OPEN_READWRITE)) {
            db.execSQL("UPDATE chunk SET data=? WHERE doc=? AND id=1 AND ordinal=?",new Object[]{last,broken.toString(),ordinal});
        }
        try(DocumentDatabase db=DocumentDatabase.open(file,false)) {
            long before=DocumentDatabase.canonicalReadCounters()[7];
            assertThrows(SQLiteException.class,()->read(db,broken,1,1,0,1));
            assertEquals(before+1,DocumentDatabase.canonicalReadCounters()[7]);noLease();
            assertEquals(healthy,db.selected());
            assertEquals(HEADER+PACKET,read(db,healthy,1,1,0,1).packet().remaining());noLease();
        }
    }
}
