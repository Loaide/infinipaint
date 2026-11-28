package com.infinipaint.storage;

import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteException;
import android.database.sqlite.SQLiteStatement;

import java.io.Closeable;
import java.io.File;
import java.io.DataInputStream;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.UUID;
import java.util.Objects;
import java.util.concurrent.CancellationException;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.BooleanSupplier;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

/**
 * Worker-confined catalog and transaction boundary. The caller supplies an app-private
 * path. Existing DBs are never initialized, upgraded, deleted or retried as new.
 * No references to an Activity, View, renderer or input state.
 */
public final class DocumentDatabase implements Closeable {
    private final SQLiteDatabase db;
    private final Thread owner;
    static final int FORMAT = 1;
    private static final int APPLICATION_ID = 0x49504443; // "IPDC"
    public static final int CHUNK_BYTES = 65520; // 1820 whole 36-byte samples

    private DocumentDatabase(SQLiteDatabase db) {
        this.db = db;
        owner = Thread.currentThread();
    }

    // Call only on the single app-lifetime storage worker. initialize is explicit.
    public static DocumentDatabase open(File path, boolean initialize) {
        if (initialize == path.exists())
            throw new SQLiteException(initialize ? "Catalog already exists" : "Catalog is missing");
        if (initialize) {
            for (String suffix : new String[]{"-wal", "-shm", "-journal"})
                if (new File(path.getPath() + suffix).exists())
                    throw new SQLiteException("Preserved catalog sidecar requires recovery");
        } else {
            // Do not let SQLite open/recover an unrelated or obviously truncated
            // file (including an existing zero-byte file) as an empty database.
            byte[] magic=new byte[16];
            try(DataInputStream in=new DataInputStream(new FileInputStream(path))) {
                in.readFully(magic);
            } catch(IOException e) {
                throw new SQLiteException("Unreadable catalog; files preserved",e);
            }
            if(!Arrays.equals(magic,"SQLite format 3\0".getBytes(StandardCharsets.US_ASCII)))
                throw new SQLiteException("Not a SQLite catalog; files preserved");
            // Check compatibility before allowing the writable connection to
            // change journal mode or run schema statements on an unknown file.
            SQLiteDatabase.OpenParams probe=new SQLiteDatabase.OpenParams.Builder()
                    .setOpenFlags(SQLiteDatabase.OPEN_READONLY)
                    .setErrorHandler(d->{throw new SQLiteException("Catalog corrupt; files preserved");}).build();
            try(SQLiteDatabase ro=SQLiteDatabase.openDatabase(path,probe)) {verifyFormat(ro);}
        }
        SQLiteDatabase.OpenParams.Builder params = new SQLiteDatabase.OpenParams.Builder()
                .setOpenFlags(SQLiteDatabase.OPEN_READWRITE | SQLiteDatabase.ENABLE_WRITE_AHEAD_LOGGING
                        | (initialize ? SQLiteDatabase.CREATE_IF_NECESSARY : 0))
                .setJournalMode(SQLiteDatabase.JOURNAL_MODE_WAL)
                .setSynchronousMode(SQLiteDatabase.SYNC_MODE_FULL)
                // Never invoke DefaultDatabaseErrorHandler (it deletes database files).
                .setErrorHandler(d -> { throw new SQLiteException("Catalog corrupt; files preserved"); });
        SQLiteDatabase db = SQLiteDatabase.openDatabase(path, params.build());
        try {
            db.setForeignKeyConstraintsEnabled(true);
            db.beginTransactionNonExclusive();
            try {
                // Check on the transaction's writer connection.
                if (!"wal".equalsIgnoreCase(scalar(db, "PRAGMA journal_mode")) ||
                        !"2".equals(scalar(db, "PRAGMA synchronous")) ||
                        !"1".equals(scalar(db, "PRAGMA foreign_keys")))
                    throw new SQLiteException("Required SQLite durability settings unavailable");
                if (initialize) createSchema(db);
                verifyFormat(db);
                db.setTransactionSuccessful();
            } finally {
                db.endTransaction();
            }
            return new DocumentDatabase(db);
        } catch (RuntimeException | Error e) {
            db.close();
            throw e;
        }
    }

    private static String scalar(SQLiteDatabase db, String sql) {
        try (Cursor c = db.rawQuery(sql, null)) {
            if (!c.moveToFirst()) throw new SQLiteException("Missing catalog value");
            return c.getString(0);
        }
    }
    private static void verifyFormat(SQLiteDatabase db) {
        if(db.getVersion()!=FORMAT || !Integer.toString(APPLICATION_ID).equals(scalar(db,"PRAGMA application_id")))
            throw new SQLiteException("Unsupported catalog; migration not performed");
    }

    private static void createSchema(SQLiteDatabase db) {
        db.execSQL("CREATE TABLE document(uuid TEXT PRIMARY KEY NOT NULL, revision INTEGER NOT NULL DEFAULT 0," +
                " ceiling INTEGER NOT NULL DEFAULT 0, head INTEGER NOT NULL DEFAULT 0," +
                " tail INTEGER NOT NULL DEFAULT 0, count INTEGER NOT NULL DEFAULT 0," +
                " last_token TEXT, view_seq INTEGER NOT NULL DEFAULT 0, pan_x REAL NOT NULL DEFAULT 0," +
                " pan_y REAL NOT NULL DEFAULT 0, zoom REAL NOT NULL DEFAULT 1," +
                " CHECK(revision>=0 AND ceiling>=0 AND count>=0 AND head>=0 AND tail>=0))");
        db.execSQL("CREATE TABLE catalog(singleton INTEGER PRIMARY KEY CHECK(singleton=1)," +
                " current TEXT REFERENCES document(uuid))");
        db.execSQL("INSERT INTO catalog VALUES(1,NULL)");
        db.execSQL("CREATE TABLE stroke(doc TEXT NOT NULL REFERENCES document(uuid)," +
                " id INTEGER NOT NULL CHECK(id>0), prev INTEGER NOT NULL CHECK(prev>=0)," +
                " next INTEGER NOT NULL CHECK(next>=0), content_rev INTEGER NOT NULL," +
                " geometry_rev INTEGER NOT NULL, header BLOB NOT NULL CHECK(length(header)<=1024)," +
                " samples INTEGER NOT NULL CHECK(samples BETWEEN 1 AND 1000000)," +
                " digest BLOB NOT NULL CHECK(length(digest)=32), PRIMARY KEY(doc,id)) WITHOUT ROWID");
        db.execSQL("CREATE TABLE chunk(doc TEXT NOT NULL, id INTEGER NOT NULL, ordinal INTEGER NOT NULL" +
                " CHECK(ordinal BETWEEN 0 AND 549), data BLOB NOT NULL CHECK(length(data) BETWEEN 36 AND " +
                CHUNK_BYTES + " AND length(data)%36=0), PRIMARY KEY(doc,id,ordinal)," +
                " FOREIGN KEY(doc,id) REFERENCES stroke(doc,id) ON DELETE CASCADE) WITHOUT ROWID");
        db.execSQL("PRAGMA application_id=" + APPLICATION_ID);
        db.setVersion(FORMAT);
    }
    /** Diagnostic evidence from this connection. A FULL checkpoint is explicit,
     * never triggered on input or as part of every autosave. */
    long[] checkpointStats() {
        checkThread();
        try(Cursor c=db.rawQuery("PRAGMA wal_checkpoint(FULL)",null)) {
            if(!c.moveToFirst()) throw new SQLiteException("No checkpoint statistics");
            return new long[]{c.getLong(0),c.getLong(1),c.getLong(2),
                    Long.parseLong(scalar(db,"PRAGMA page_size")),Long.parseLong(scalar(db,"PRAGMA cache_size"))};
        }
    }

    private void checkThread() {
        if (Thread.currentThread()!=owner) throw new IllegalStateException("Not storage worker");
        if (db.inTransaction()) throw new IllegalStateException("Reentrant catalog operation");
    }

    public UUID createDocument() {
        checkThread();
        // INSERT OR IGNORE applies here only to the UUID uniqueness constraint.
        try (SQLiteStatement s = db.compileStatement("INSERT OR IGNORE INTO document(uuid) VALUES(?)")) {
            for (int i=0;i<8;i++) {
                UUID id=UUID.randomUUID();
                s.bindString(1,id.toString());
                if (s.executeInsert()!=-1) return id;
            }
        }
        throw new SQLiteException("Could not allocate a unique document identity");
    }

    /** Returns inclusive start; commit of this reservation precedes use of any ID. */
    public long reserve(UUID id, int number) {
        checkThread();
        if (number<=0) throw new IllegalArgumentException("Invalid reservation");
        String key=id.toString();
        db.beginTransactionNonExclusive();
        try (Cursor c=db.rawQuery("SELECT ceiling FROM document WHERE uuid=?", new String[]{key})) {
            if (!c.moveToFirst()) throw new SQLiteException("Unknown document");
            long end=c.getLong(0);
            if(end<0 || end>Long.MAX_VALUE-number) throw new SQLiteException("Stroke ID range exhausted");
            db.execSQL("UPDATE document SET ceiling=? WHERE uuid=?", new Object[]{end+number,key});
            db.setTransactionSuccessful();
            return end+1;
        } finally { db.endTransaction(); }
    }

    /** Called after successful validation/load, never as part of probing a candidate. */
    public void select(UUID id) {
        checkThread();
        db.execSQL("UPDATE catalog SET current=? WHERE singleton=1",new Object[]{id.toString()});
    }

    public UUID selected() {
        checkThread();
        String s=scalar(db,"SELECT current FROM catalog WHERE singleton=1");
        return s==null?null:UUID.fromString(s);
    }
    public UUID next(UUID after) {
        checkThread();
        String key=after==null?"":after.toString();
        try(Cursor c=db.rawQuery("SELECT uuid FROM document WHERE uuid>? ORDER BY uuid LIMIT 1",new String[]{key})) {
            if(c.moveToFirst()) return UUID.fromString(c.getString(0));
        }
        try(Cursor c=db.rawQuery("SELECT uuid FROM document ORDER BY uuid LIMIT 1",null)) {
            return c.moveToFirst()?UUID.fromString(c.getString(0)):null;
        }
    }

    public static final class View {
        public final long seq;
        public final double x,y,zoom;
        private View(Cursor c) {
            seq=c.getLong(0); x=c.getDouble(1); y=c.getDouble(2); zoom=c.getDouble(3);
            if (seq<0 || !Double.isFinite(x) || !Double.isFinite(y) || !(zoom>=.1 && zoom<=16))
                throw new SQLiteException("Invalid stored camera");
        }
    }
    public View view(UUID id) {
        checkThread();
        try (Cursor c=db.rawQuery("SELECT view_seq,pan_x,pan_y,zoom FROM document WHERE uuid=?",
                new String[]{id.toString()})) {
            if (!c.moveToFirst()) throw new SQLiteException("Unknown document");
            return new View(c);
        }
    }

    /** Streaming boundary: a source supplies only the requested chunk; returned arrays
     * are consumed before requesting another one. Native codec validates every value. */
    public interface ChunkSource { byte[] read(int ordinal); }
    public interface Changes { void write(Writer writer); }
    public static final class Writer {
        private SQLiteDatabase transaction;
        private final String key;
        private final long revision;
        private long head,tail,count,ceiling;
        private boolean failed;
        private Writer(SQLiteDatabase tx,String key,long revision,long head,long tail,long count,long ceiling) {
            transaction=tx; this.key=key; this.revision=revision;
            this.head=head; this.tail=tail; this.count=count; this.ceiling=ceiling;
        }
        private SQLiteDatabase tx() {
            if (transaction==null || !transaction.inTransaction() || failed)
                throw new IllegalStateException("Writer used outside its transaction");
            return transaction;
        }
        public void remove(long id) {
            SQLiteDatabase db=tx();
            failed=true;
            long[] links=links(db,id);
            if(db.delete("stroke","doc=? AND id=?",new String[]{key,Long.toString(id)})!=1)
                throw new SQLiteException("Missing stroke");
            if(links[0]==0) head=links[1]; else setLink(db,links[0],true,links[1]);
            if(links[1]==0) tail=links[0]; else setLink(db,links[1],false,links[0]);
            --count;
            failed=false;
        }
        public void removeIfPresent(long id) {
            SQLiteDatabase db=tx();
            try(Cursor c=db.rawQuery("SELECT 1 FROM stroke WHERE doc=? AND id=?",new String[]{key,Long.toString(id)})) {
                if(c.moveToFirst()) remove(id);
            }
        }
        public void insertAfter(long id,long prev,byte[] header,int count,ChunkSource source) {
            insert(id,prev,prev==0?head:links(tx(),prev)[1],header,count,source);
        }
        void restoreGeometryRevision(long id,long geometry) {
            if(geometry<=0 || geometry>revision) throw new SQLiteException("Invalid geometry revision");
            if(geometry!=revision) tx().execSQL("UPDATE stroke SET geometry_rev=? WHERE doc=? AND id=?",
                    new Object[]{geometry,key,id});
        }
        public void clear() {
            SQLiteDatabase db=tx(); failed=true;
            db.delete("stroke","doc=?",new String[]{key});
            head=tail=count=0; failed=false;
        }
        private long[] links(SQLiteDatabase db,long id) {
            try(Cursor c=db.rawQuery("SELECT prev,next FROM stroke WHERE doc=? AND id=?",
                    new String[]{key,Long.toString(id)})) {
                if(!c.moveToFirst()) throw new SQLiteException("Missing order neighbor");
                return new long[]{c.getLong(0),c.getLong(1)};
            }
        }
        private void setLink(SQLiteDatabase db,long id,boolean forward,long value) {
            try (SQLiteStatement s=db.compileStatement("UPDATE stroke SET "+(forward?"next":"prev")+"=? WHERE doc=? AND id=?")) {
                s.bindLong(1,value); s.bindString(2,key); s.bindLong(3,id);
                if(s.executeUpdateDelete()!=1) throw new SQLiteException("Missing stroke to relink");
            }
        }
        public void insert(long id,long prev,long next,byte[] header,int count,ChunkSource source) {
            SQLiteDatabase db=tx(); failed=true;
            if(header.length!=80 || count<=0 || count>1000000 || id<=0 || id>ceiling)
                throw new IllegalArgumentException("Invalid record");
            if((prev==0?head:links(db,prev)[1])!=next || (next==0?tail:links(db,next)[0])!=prev)
                throw new SQLiteException("Invalid insertion slot");
            MessageDigest hash=sha();
            hash.update(header);
            db.execSQL("INSERT INTO stroke VALUES(?,?,?,?,?,?,?,?,zeroblob(32))",
                    new Object[]{key,id,prev,next,revision,revision,header,count});
            try(SQLiteStatement s=db.compileStatement("INSERT INTO chunk VALUES(?,?,?,?)")) {
                s.bindString(1,key); s.bindLong(2,id);
                int remaining=count*36;
                for(int i=0;remaining>0;i++) {
                    byte[] b=source.read(i);
                    if(b.length!=Math.min(remaining,CHUNK_BYTES)) throw new IllegalArgumentException("Invalid chunk size");
                    s.bindLong(3,i); s.bindBlob(4,b); s.executeInsert();
                    hash.update(b); remaining-=b.length;
                }
            }
            db.execSQL("UPDATE stroke SET digest=? WHERE doc=? AND id=?",new Object[]{hash.digest(),key,id});
            if(prev==0) head=id; else setLink(db,prev,true,id);
            if(next==0) tail=id; else setLink(db,next,false,id);
            this.count++; failed=false;
        }
    }

    private static MessageDigest sha() {
        try { return MessageDigest.getInstance("SHA-256"); }
        catch(NoSuchAlgorithmException e) { throw new AssertionError(e); }
    }

    /** Returns only after complete group commit. A thrown exception never authorizes a durable ack. */
    public void commit(UUID id,long base,long head,long tail,long count,Changes changes) {
        commit(id,UUID.randomUUID(),base,head,tail,count,changes);
    }
    public void commit(UUID id,UUID token,long base,long head,long tail,long count,Changes changes) {
        checkThread();
        if(base<0 || base==Long.MAX_VALUE || count<0 ||
                (count==0 ? head!=0 || tail!=0 : head<=0 || tail<=0))
            throw new IllegalArgumentException("Invalid document change");
        String key=id.toString();
        db.beginTransactionNonExclusive();
        Writer w=null;
        try(Cursor c=db.rawQuery("SELECT head,tail,count,ceiling,revision,last_token FROM document WHERE uuid=?",new String[]{key});
            SQLiteStatement s=db.compileStatement("UPDATE document SET revision=?,head=?,tail=?,count=?,last_token=?" +
                " WHERE uuid=? AND revision=? AND ceiling>=?")) {
            if(!c.moveToFirst()) throw new SQLiteException("Missing document");
            if(c.getLong(4)==base+1 && token.toString().equals(c.getString(5))) {
                db.setTransactionSuccessful(); return;
            }
            w=new Writer(db,key,base+1,c.getLong(0),c.getLong(1),c.getLong(2),c.getLong(3));
            s.bindLong(1,base+1); s.bindLong(2,head); s.bindLong(3,tail); s.bindLong(4,count);
            s.bindString(5,token.toString()); s.bindString(6,key); s.bindLong(7,base); s.bindLong(8,Math.max(head,tail));
            if(s.executeUpdateDelete()!=1) throw new SQLiteException("Document revision/identity conflict");
            changes.write(w);
            if(w.failed || w.head!=head || w.tail!=tail || w.count!=count)
                throw new SQLiteException("Incomplete document change");
            db.setTransactionSuccessful();
        } finally {
            if(w!=null) w.transaction=null;
            db.endTransaction();
        }
    }

    /** Consumer builds an isolated candidate, never the current document. Discard it
     * on ANY exception, including a digest mismatch after the final chunk. */
    public interface RecordSink {
        void stroke(long id,long prev,long next,long content,long geometry,int samples,byte[] header);
        void chunk(byte[] bytes);
        void endStroke();
    }
    public interface Restore extends RecordSink {
        void begin(UUID id,long revision,long ceiling,long count);
    }
    // Cumulative process counters. Full-load successes mean a verified record,
    // not that the entire document was subsequently activated.
    private static final AtomicLong[] READS = new AtomicLong[12];
    static {for(int i=0;i<READS.length;i++) READS[i]=new AtomicLong();}
    public static long[] canonicalReadCounters() {
        long[] values=new long[READS.length];
        for(int i=0;i<values.length;i++) values[i]=READS[i].get();
        return values;
    }
    private static void checkCancelled(BooleanSupplier cancelled) {
        if(cancelled.getAsBoolean()) throw new CancellationException("Obsolete canonical request");
    }
    private static final class RecordRead {
        final long prev,next,bytes;
        RecordRead(long prev,long next,long bytes) {this.prev=prev;this.next=next;this.bytes=bytes;}
    }
    private RecordRead streamRecord(String key,long record,long previous,long rev,long ceiling,
            long expectedContent,long expectedGeometry,long maxSampleBytes,
            BooleanSupplier cancelled,RecordSink into) {
        if(record<=0 || record>ceiling) throw new SQLiteException("Invalid linked identity");
        String[] args={key,Long.toString(record)};
        checkCancelled(cancelled);
        READS[0].incrementAndGet();
        try(Cursor c=db.rawQuery("SELECT prev,next,content_rev,geometry_rev,samples,length(header)," +
                "substr(header,1,80),length(digest),substr(digest,1,32) FROM stroke WHERE doc=? AND id=?",args)) {
            if(!c.moveToFirst() || (previous>=0 && c.getLong(0)!=previous))
                throw new SQLiteException("Broken order chain");
            long prev=c.getLong(0),next=c.getLong(1),content=c.getLong(2),geometry=c.getLong(3),n=c.getLong(4);
            if(prev<0 || prev>ceiling || next<0 || next>ceiling || prev==record || next==record ||
                    content<=0 || content>rev || geometry<=0 || geometry>content || n<=0 ||
                    n>1000000 || n*36>maxSampleBytes || c.getLong(5)!=80 || c.getLong(7)!=32)
                throw new SQLiteException("Invalid stroke header or load budget");
            if(expectedContent>0 && (content!=expectedContent || geometry!=expectedGeometry)) {
                READS[8].incrementAndGet();throw new SQLiteException("Stale stroke revisions");
            }
            byte[] header=c.getBlob(6),digest=c.getBlob(8);
            if(header.length!=80 || digest.length!=32) throw new SQLiteException("Malformed header blob");
            MessageDigest hash=sha();hash.update(header);
            into.stroke(record,prev,next,content,geometry,(int)n,header);
            long remaining=n*36;
            try(Cursor chunks=db.rawQuery("SELECT ordinal,length(data),substr(data,1,"+CHUNK_BYTES+
                    ") FROM chunk WHERE doc=? AND id=? ORDER BY ordinal",args)) {
                int ordinal=0;
                while(chunks.moveToNext()) {
                    checkCancelled(cancelled);
                    if(remaining==0 || chunks.getLong(0)!=ordinal++ ||
                            chunks.getLong(1)!=Math.min(remaining,CHUNK_BYTES))
                        throw new SQLiteException("Malformed sample chunk");
                    byte[] data=chunks.getBlob(2);
                    if(data.length!=Math.min(remaining,CHUNK_BYTES)) throw new SQLiteException("Malformed sample blob");
                    hash.update(data);READS[2].incrementAndGet();READS[3].addAndGet(data.length);
                    into.chunk(data);remaining-=data.length;
                }
            }
            if(remaining!=0 || !MessageDigest.isEqual(hash.digest(),digest)) {
                READS[7].incrementAndGet();throw new SQLiteException("Stroke checksum or size mismatch");
            }
            checkCancelled(cancelled);into.endStroke();READS[1].incrementAndGet();
            return new RecordRead(prev,next,n*36);
        }
    }
    public void load(UUID id,long maxRecords,long maxSampleBytes,Restore into) {
        checkThread();
        String key=id.toString();
        db.beginTransactionNonExclusive(); // single writer, coherent snapshot
        try(Cursor meta=db.rawQuery("SELECT revision,ceiling,head,tail,count FROM document WHERE uuid=?",
                new String[]{key})) {
            if(!meta.moveToFirst()) throw new SQLiteException("Unknown document");
            long rev=meta.getLong(0), ceiling=meta.getLong(1), head=meta.getLong(2);
            long tail=meta.getLong(3), count=meta.getLong(4);
            if(rev<0 || ceiling<0 || count<0 || count>maxRecords || maxSampleBytes<0 ||
                    (count==0 ? head!=0 || tail!=0 : head<=0 || tail<=0))
                throw new SQLiteException("Invalid or oversized document");
            try(Cursor totals=db.rawQuery("SELECT count(*) FROM stroke WHERE doc=?",new String[]{key})) {
                if(!totals.moveToFirst() || totals.getLong(0)!=count) throw new SQLiteException("Unlinked records");
            }
            into.begin(id,rev,ceiling,count);
            long record=head,previous=0;
            // Complete validation remains mandatory before document selection.
            for(long i=0;i<count;i++) {
                RecordRead read=streamRecord(key,record,previous,rev,ceiling,0,0,maxSampleBytes,()->false,into);
                maxSampleBytes-=read.bytes;previous=record;record=read.next;
            }
            if(record!=0 || previous!=tail) throw new SQLiteException("Invalid document tail");
            db.setTransactionSuccessful();
        } finally { db.endTransaction(); }
    }
    /** Targeted read of a single canonical record under an exact durable revision.
     * Does NOT validate the entire document/order root. The caller must already
     * have validated that root and must discard the sink on ANY failure. Pending
     * visible edits are not part of this durable snapshot. */
    void readStroke(UUID doc,long revision,long id,long content,long geometry,long maxSampleBytes,
                    BooleanSupplier cancelled,RecordSink into) {
        checkThread();Objects.requireNonNull(doc);Objects.requireNonNull(cancelled);Objects.requireNonNull(into);
        if(revision<=0||id<=0||content<=0||content>revision||geometry<=0||geometry>content||maxSampleBytes<36)
            throw new IllegalArgumentException("Invalid canonical read request");
        READS[4].incrementAndGet();long start=System.nanoTime();
        try {
            checkCancelled(cancelled);
            String key=doc.toString();
            db.beginTransactionNonExclusive();
            try(Cursor meta=db.rawQuery("SELECT revision,ceiling,head,tail,count FROM document WHERE uuid=?",new String[]{key})) {
                if(!meta.moveToFirst()||meta.getLong(0)!=revision) {
                    READS[8].incrementAndGet();throw new SQLiteException("Stale document revision");
                }
                long ceiling=meta.getLong(1),head=meta.getLong(2),tail=meta.getLong(3);
                if(ceiling<id||head<=0||head>ceiling||tail<=0||tail>ceiling||meta.getLong(4)<=0)
                    throw new SQLiteException("Invalid document root");
                RecordRead read=streamRecord(key,id,-1,revision,ceiling,content,geometry,maxSampleBytes,cancelled,into);
                // A targeted read also checks its immediate reciprocal links.
                // Complete chain validation remains the responsibility of load.
                if((read.prev==0)!=(head==id)||(read.next==0)!=(tail==id)||
                        (read.prev!=0&&!linked(key,read.prev,"next",id))||
                        (read.next!=0&&!linked(key,read.next,"prev",id)))
                    throw new SQLiteException("Invalid canonical order neighbor");
                checkCancelled(cancelled);db.setTransactionSuccessful();
            } finally {db.endTransaction();}
            checkCancelled(cancelled);READS[5].incrementAndGet();
        } catch(CancellationException e) {READS[9].incrementAndGet();throw e;}
          catch(RuntimeException | Error e) {READS[6].incrementAndGet();throw e;}
        finally {long ns=System.nanoTime()-start;READS[10].addAndGet(ns);READS[11].accumulateAndGet(ns,Math::max);}
    }

    private boolean linked(String doc,long id,String column,long expected) {
        try(Cursor c=db.rawQuery("SELECT "+column+" FROM stroke WHERE doc=? AND id=?",
                new String[]{doc,Long.toString(id)})) {return c.moveToFirst()&&c.getLong(0)==expected;}
    }
    public static long[] canonicalRangeCounters() {return StorePort.rangeCounters();}
    /** One verified, bounded spline-interval page. The exact revision and
     * geometry identity travel with the data. Consumers must revalidate the
     * desired scene before publishing it. No pending/unsaved edits are read.
     * The final packet of a stroke is its terminal connector/dab. */
    static final class CurveRange {
        final UUID doc;
        final long revision,id,content,geometry;
        private final byte[] data;
        CurveRange(UUID doc,long revision,long id,long content,long geometry,byte[] data) {
            this.doc=doc;this.revision=revision;this.id=id;this.content=content;this.geometry=geometry;
            this.data=Objects.requireNonNull(data);
        }
        java.nio.ByteBuffer packet() {
            return java.nio.ByteBuffer.wrap(data).asReadOnlyBuffer().order(java.nio.ByteOrder.LITTLE_ENDIAN);
        }
    }
    /** Requested interval ordinals are independent of input chunk boundaries.
     * The v1 filter is stateful, so this implementation replays the requested
     * stroke from its beginning, never the entire document. No persistent
     * checkpoints or page-addressed coefficient cache yet. */
    CurveRange readCurveRange(UUID doc,long revision,long id,long content,long geometry,
                              int first,int count,long maxSampleBytes,BooleanSupplier cancelled) {
        checkThread();
        if(first<0||first>1000000||count<=0||count>128||(long)first+count>1000001)
            throw new IllegalArgumentException("Invalid curve range");
        final long[] token={0};final byte[][] packet={null};
        try {
            readStroke(doc,revision,id,content,geometry,maxSampleBytes,cancelled,new RecordSink() {
                public void stroke(long i,long prev,long next,long cr,long gr,int samples,byte[] header) {
                    token[0]=StorePort.rangeBegin(header,samples,first,count);
                    if(token[0]<=0) throw new IllegalStateException("Curve range admission failed");
                }
                public void chunk(byte[] bytes) {
                    // Check for obsolete intent between bounded decode slices;
                    // no additional Java chunk copy and no time-limit claim.
                    final int slice=64*36;
                    for(int offset=0;offset<bytes.length;offset+=slice) {
                        checkCancelled(cancelled);
                        StorePort.rangeChunk(token[0],bytes,offset,Math.min(slice,bytes.length-offset));
                    }
                }
                public void endStroke() {packet[0]=Objects.requireNonNull(StorePort.rangeFinish(token[0]));}
            });
            return new CurveRange(doc,revision,id,content,geometry,packet[0]);
        } finally {if(token[0]!=0) StorePort.rangeDiscard(token[0]);}
    }

    /** Caller's coalescer supplies monotonically increasing per-document view sequence. */
    public boolean saveView(UUID id,long seq,double x,double y,double zoom) {
        checkThread();
        if (seq<=0 || !Double.isFinite(x) || !Double.isFinite(y) || !(zoom>=.1 && zoom<=16))
            throw new IllegalArgumentException("Invalid view");
        try(SQLiteStatement s=db.compileStatement("UPDATE document SET view_seq=?,pan_x=?,pan_y=?,zoom=?" +
                " WHERE uuid=? AND view_seq<?")) {
            s.bindLong(1,seq); s.bindDouble(2,x); s.bindDouble(3,y); s.bindDouble(4,zoom);
            s.bindString(5,id.toString()); s.bindLong(6,seq);
            return s.executeUpdateDelete()==1;
        }
    }
    @Override public void close() { checkThread(); db.close(); }
}
