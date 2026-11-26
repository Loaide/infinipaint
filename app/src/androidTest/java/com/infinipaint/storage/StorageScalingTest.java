package com.infinipaint.storage;

import android.content.Context;
import android.os.SystemClock;
import android.util.Log;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Arrays;
import java.util.UUID;
import org.junit.Test;
import org.junit.Assume;
import static org.junit.Assert.*;

/** Named opt-in device probe, measured outputs go to IP_M03_SQL rather than
 * comparing dissimilar phones against a hardcoded elapsed-time assertion. */
public final class StorageScalingTest {
    private static long writeBytes() {
        try {
            for(String s:Files.readAllLines(Paths.get("/proc/self/io"))) if(s.startsWith("write_bytes:"))
                return Long.parseLong(s.substring(12).trim());
        } catch(Exception unavailable) { /* SELinux/kernel may not expose counters */ }
        return -1;
    }
    private static String distribution(long[] a) {
        Arrays.sort(a);return a[a.length/2]+"/"+a[(a.length*95-1)/100]+"/"+a[(a.length*99-1)/100]+"/"+a[a.length-1];
    }
    @Test public void oneDotAfterLargerCatalogs() throws Exception {
        Assume.assumeTrue("Pass -e m03Scale true for storage measurements",
                "true".equals(InstrumentationRegistry.getArguments().getString("m03Scale")));
        Context ctx=InstrumentationRegistry.getInstrumentation().getTargetContext();
        assertTrue(ctx.getPackageName().endsWith(".m03verify"));
        for(int size:new int[]{1000,10000,100000}) {
            File dir=Files.createTempDirectory(ctx.getCacheDir().toPath(),"sql-scale-").toFile();
            File path=new File(dir,"probe.sqlite");
            byte[] h=new byte[80],sample=new byte[36]; // workload probes page writes, not ink decoding
            try(DocumentDatabase db=DocumentDatabase.open(path,true)) {
                UUID doc=db.createDocument();db.reserve(doc,size+64);
                db.commit(doc,0,1,size,size,w->{
                    for(int i=1;i<=size;i++) w.insertAfter(i,i-1,h,1,j->sample);
                });
                long[] times=new long[31],writes=new long[31],checkpoints=new long[31],walFrames=new long[31];
                long[] initial=db.checkpointStats();assertEquals(0,initial[0]);
                for(int i=0;i<32;i++) {
                    int id=size+i+1;
                    long before=writeBytes(),start=SystemClock.elapsedRealtimeNanos();
                    db.commit(doc,i+1,1,id,id,w->w.insertAfter(id,id-1,h,1,j->sample));
                    long committed=SystemClock.elapsedRealtimeNanos();
                    long[] stats=db.checkpointStats();assertEquals(0,stats[0]);
                    long end=SystemClock.elapsedRealtimeNanos(),after=writeBytes();
                    if(i>0) {
                        times[i-1]=committed-start;checkpoints[i-1]=end-committed;
                        writes[i-1]=before<0 || after<0?-1:after-before;walFrames[i-1]=stats[1];
                    }
                }
                long[] loads=new long[5];
                for(int run=0;run<=loads.length;run++) {
                    int[] rows={0};long start=SystemClock.elapsedRealtimeNanos();
                    db.load(doc,size+32L,(size+32L)*36,new DocumentDatabase.Restore() {
                        public void begin(UUID id,long rev,long ceiling,long count){assertEquals(size+32,count);}
                        public void stroke(long id,long prev,long next,long content,long geometry,int n,byte[] h){}
                        public void chunk(byte[] b){assertEquals(36,b.length);}
                        public void endStroke(){rows[0]++;}
                    });
                    assertEquals(size+32,rows[0]);
                    if(run>0) loads[run-1]=SystemClock.elapsedRealtimeNanos()-start;
                }
                Log.i("IP_M03_SQL","size="+size+" n=31 warmed p50/p95/p99/max commit_ns="+distribution(times)+
                        " checkpoint_ns="+distribution(checkpoints)+" proc_write_bytes_including_checkpoint="+distribution(writes)+
                        " wal_frames="+distribution(walFrames)+" page_size="+initial[3]+" cache_setting="+initial[4]+
                        " db_bytes="+path.length()+" wal_bytes="+new File(path+"-wal").length()+
                        " validated_sql_load_n5_ns="+distribution(loads));
            }
            File[] files=dir.listFiles();assertNotNull(files);for(File f:files) assertTrue(f.delete());assertTrue(dir.delete());
        }
    }
}
