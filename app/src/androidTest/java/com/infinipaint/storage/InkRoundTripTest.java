package com.infinipaint.storage;

import android.content.Context;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import androidx.test.platform.app.InstrumentationRegistry;
import com.infinipaint.demo.NativeBridge;
import org.junit.Test;
import java.io.File;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.util.UUID;
import static org.junit.Assert.*;

/** Device-only. Uses the actual JNI writer, platform SQLite and GLES2 driver. */
public final class InkRoundTripTest {
    private static final int W=96,H=80;
    private static final long LIMIT=384L<<20;
    private static byte[] pixels(int mode) {
        NativeBridge.render(mode);
        ByteBuffer b=ByteBuffer.allocateDirect(W*H*4);
        GLES20.glReadPixels(0,0,W,H,GLES20.GL_RGBA,GLES20.GL_UNSIGNED_BYTE,b);
        assertEquals(GLES20.GL_NO_ERROR,GLES20.glGetError());
        byte[] result=new byte[b.capacity()];b.get(result);return result;
    }
    private static void flush(DocumentDatabase db,UUID id) {
        long[] g;
        while((g=StorePort.delta(id.getMostSignificantBits(),id.getLeastSignificantBits()))!=null) {
            UUID token=UUID.randomUUID();
            EditorStore.persist(db,id,token,g);
            // Ambiguous acknowledgement: exactly the same token must not apply twice.
            EditorStore.persist(db,id,token,g);
            StorePort.ack();
        }
    }
    private static void command(int op,double[] path) {
        NativeBridge.suspend(true);
        assertEquals(0,NativeBridge.command(op,path,1));
        NativeBridge.suspend(false);
    }
    @Test public void inkPixelsAndGroupedEditsSurviveActualDatabaseReopen() throws Exception {
        Context ctx=InstrumentationRegistry.getInstrumentation().getTargetContext();
        assertTrue(ctx.getPackageName().endsWith(".m03verify"));
        File directory=Files.createTempDirectory(ctx.getCacheDir().toPath(),"ink-roundtrip-").toFile();
        File path=new File(directory,"fixture.sqlite");
        EGLDisplay display=EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        assertTrue(EGL14.eglInitialize(display,new int[1],0,new int[1],0));
        EGLConfig[] configs=new EGLConfig[1];
        int[] cfg={EGL14.EGL_SURFACE_TYPE,EGL14.EGL_PBUFFER_BIT,EGL14.EGL_RENDERABLE_TYPE,EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_RED_SIZE,8,EGL14.EGL_GREEN_SIZE,8,EGL14.EGL_BLUE_SIZE,8,EGL14.EGL_ALPHA_SIZE,8,EGL14.EGL_NONE};
        assertTrue(EGL14.eglChooseConfig(display,cfg,0,configs,0,1,new int[1],0));assertNotNull(configs[0]);
        EGLContext gl=EGL14.eglCreateContext(display,configs[0],EGL14.EGL_NO_CONTEXT,
                new int[]{EGL14.EGL_CONTEXT_CLIENT_VERSION,2,EGL14.EGL_NONE},0);
        EGLSurface surface=EGL14.eglCreatePbufferSurface(display,configs[0],
                new int[]{EGL14.EGL_WIDTH,W,EGL14.EGL_HEIGHT,H,EGL14.EGL_NONE},0);
        try {
            assertTrue(EGL14.eglMakeCurrent(display,surface,surface,gl));
            NativeBridge.contextCreated();NativeBridge.resizeViewport(W,H);
            UUID id,other;byte[][] expected=new byte[4][];long revision;
            try(DocumentDatabase db=DocumentDatabase.open(path,true)) {
                NativeBridge.suspend(true);
                id=db.createDocument();EditorStore.activate(db,id,LIMIT,db.view(id));
                other=db.createDocument();assertTrue(db.saveView(other,1,250,600,2));
                NativeBridge.suspend(false);
                byte[] blank=pixels(2);
                for(int stroke=0;stroke<2;stroke++) {
                    assertTrue(NativeBridge.begin(stroke,2));
                    int count=stroke==0?41633:12;
                    for(int i=0;i<count;i++) {
                        double x=stroke==0?22+(i%2):65+i, y=stroke==0?32:49;
                        assertTrue(NativeBridge.sample(stroke,x,y,1+(i/2)*.001,(i%11)/10f,.2f,-.1f));
                    }
                    assertTrue(NativeBridge.end(stroke));flush(db,id);
                }
                android.util.Log.i("IP_M03_MEMORY","long_fixture="+java.util.Arrays.toString(NativeBridge.memory())+
                        " native_allocated="+android.os.Debug.getNativeHeapAllocatedSize());
                byte[] painted=pixels(2);
                assertFalse(java.util.Arrays.equals(blank,painted));
                command(4,new double[]{17,32,28,32});flush(db,id);
                byte[] erased=pixels(2);
                assertFalse(java.util.Arrays.equals(erased,painted));assertFalse(java.util.Arrays.equals(erased,blank));
                command(1,null);flush(db,id);assertArrayEquals(painted,pixels(2));
                command(2,null);flush(db,id);assertArrayEquals(erased,pixels(2));
                command(1,null);flush(db,id);
                command(3,null);flush(db,id);assertArrayEquals(blank,pixels(2));
                command(1,null);flush(db,id);
                revision=NativeBridge.revision();
                for(int m=0;m<4;m++) expected[m]=pixels(m);
            }
            // Fail at catalog selection *after* the candidate is decoded and staged.
            try(android.database.sqlite.SQLiteDatabase raw=android.database.sqlite.SQLiteDatabase.openDatabase(
                    path.getPath(),null,android.database.sqlite.SQLiteDatabase.OPEN_READWRITE)) {
                raw.execSQL("CREATE TRIGGER fail_select BEFORE UPDATE ON catalog WHEN NEW.current='"+other+
                        "' BEGIN SELECT RAISE(ABORT,'injected select failure'); END");
            }
            try(DocumentDatabase reopened=DocumentDatabase.open(path,false)) {
                NativeBridge.suspend(true);
                assertEquals(id,reopened.selected());
                assertThrows(android.database.sqlite.SQLiteException.class,()->
                        EditorStore.activate(reopened,other,LIMIT,reopened.view(other)));
                assertEquals(id,reopened.selected());
                assertEquals(revision,NativeBridge.revision());
                assertArrayEquals(expected[2],pixels(2)); // neither scene nor camera published
                assertEquals(revision,EditorStore.activate(reopened,id,LIMIT,reopened.view(id)));
                for(int m=0;m<4;m++) assertArrayEquals("mode "+m,expected[m],pixels(m));
                assertEquals(6,NativeBridge.command(1,null,0)); // reopened session has no undo
            }
        } finally {
            NativeBridge.suspend(false);
            EGL14.eglMakeCurrent(display,EGL14.EGL_NO_SURFACE,EGL14.EGL_NO_SURFACE,EGL14.EGL_NO_CONTEXT);
            EGL14.eglDestroyContext(display,gl);EGL14.eglDestroySurface(display,surface);EGL14.eglTerminate(display);
        }
        File[] files=directory.listFiles();assertNotNull(files);
        for(File f:files) assertTrue(f.delete());assertTrue(directory.delete());
    }
}
