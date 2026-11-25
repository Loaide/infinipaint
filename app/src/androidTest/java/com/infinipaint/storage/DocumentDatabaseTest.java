package com.infinipaint.storage;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertThrows;
import static org.junit.Assert.assertTrue;

import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteException;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.UUID;

@RunWith(AndroidJUnit4.class)
public final class DocumentDatabaseTest {
    private static final String[] SIDECARS = {"-wal", "-shm", "-journal"};
    private static final UUID UNKNOWN = new UUID(0, 0); // Cannot be produced by UUID.randomUUID().

    private Path directory;
    private File catalog;

    @Before
    public void createScratchDirectory() throws IOException {
        Path cache = InstrumentationRegistry.getInstrumentation().getTargetContext()
                .getCacheDir().toPath();
        directory = Files.createTempDirectory(cache, "document-db-test-");
        catalog = directory.resolve("catalog.db").toFile();
    }

    @After
    public void removeScratchDirectory() throws IOException {
        if (directory == null) return;
        // All fixtures are flat files. Never recurse into or clear the app cache itself.
        try (var entries = Files.newDirectoryStream(directory)) {
            for (Path entry : entries) Files.delete(entry);
        }
        Files.delete(directory);
    }

    @Test
    public void missingCatalogRequiresExplicitInitializationAndExistingCatalogCannotBeRecreated()
            throws IOException {
        assertOpenRejected(catalog, false);
        assertFalse(catalog.exists());
        for (String suffix : SIDECARS) assertFalse(Files.exists(sidecar(catalog, suffix)));

        UUID id;
        try (DocumentDatabase db = DocumentDatabase.open(catalog, true)) {
            assertNull(db.selected());
            id = db.createDocument();
            assertEquals(4, id.version());
            assertEquals(2, id.variant());
            assertNull("Creation alone must not change selection", db.selected());
            db.select(id);
        }

        byte[] before = Files.readAllBytes(catalog.toPath());
        assertOpenRejected(catalog, true);
        assertArrayEquals(before, Files.readAllBytes(catalog.toPath()));
        try (DocumentDatabase db = DocumentDatabase.open(catalog, false)) {
            assertEquals(id, db.selected());
        }
    }

    @Test
    public void eachOrphanedSidecarBlocksNewCatalogAndSurvivesBothOpenModes() throws IOException {
        for (String suffix : SIDECARS) {
            File path = directory.resolve("orphan" + suffix + ".db").toFile();
            Path sidecar = sidecar(path, suffix);
            byte[] original = sentinel(suffix);
            Files.write(sidecar, original);

            assertOpenRejected(path, false);
            assertFalse(path.exists());
            assertArrayEquals(suffix, original, Files.readAllBytes(sidecar));

            assertOpenRejected(path, true);
            assertFalse(path.exists());
            assertArrayEquals(suffix, original, Files.readAllBytes(sidecar));
        }
    }

    @Test
    public void corruptCatalogAndSidecarsAreNotDeletedOrOverwritten() throws IOException {
        byte[] corrupt = new byte[4096]; // Not a SQLite database header.
        Files.write(catalog.toPath(), corrupt);
        for (String suffix : SIDECARS) Files.write(sidecar(catalog, suffix), sentinel(suffix));

        assertOpenRejected(catalog, true);
        assertCorruptFixtureUnchanged(corrupt);
        assertOpenRejected(catalog, false);
        assertCorruptFixtureUnchanged(corrupt);
    }

    @Test
    public void selectionPersistsAndUnknownSelectionDoesNotReplaceIt() {
        UUID first;
        UUID second;
        try (DocumentDatabase db = DocumentDatabase.open(catalog, true)) {
            first = db.createDocument();
            db.select(first);
            second = db.createDocument();
            assertNotEquals(first, second);
            assertEquals(first, db.selected());
        }
        try (DocumentDatabase db = DocumentDatabase.open(catalog, false)) {
            assertEquals(first, db.selected());
            assertThrows(SQLiteException.class, () -> db.select(UNKNOWN));
            assertEquals(first, db.selected());
            db.select(second);
            assertEquals(second, db.selected());
        }
        try (DocumentDatabase db = DocumentDatabase.open(catalog, false)) {
            assertEquals(second, db.selected());
        }
    }

    @Test
    public void viewSequenceRejectsDuplicateAndStaleWritesAcrossReopenPerDocument() {
        UUID first;
        UUID second;
        try (DocumentDatabase db = DocumentDatabase.open(catalog, true)) {
            first = db.createDocument();
            second = db.createDocument();
            db.select(first);
            assertTrue(db.saveView(first, 7, 12.5, -4.25, 2));
            assertFalse(db.saveView(first, 7, 91, 92, 3));
            assertFalse(db.saveView(first, 6, 93, 94, 4));
            assertTrue(db.saveView(second, 1, -20, 30, .5));
        }
        try (DocumentDatabase db = DocumentDatabase.open(catalog, false)) {
            assertFalse(db.saveView(first, 7, 95, 96, 5));
            assertFalse(db.saveView(first, 6, 97, 98, 6));
            assertFalse(db.saveView(UNKNOWN, 8, 0, 0, 1));
            assertEquals(first, db.selected());
        }
        assertStoredViews(first, 7, 12.5, -4.25, 2, second);

        try (DocumentDatabase db = DocumentDatabase.open(catalog, false)) {
            assertTrue(db.saveView(first, 8, 0.25, -0.5, 8));
        }
        assertStoredViews(first, 8, 0.25, -0.5, 8, second);
    }

    @Test
    public void reservedButUnusedRangesAreSkippedAcrossReopenAndScopedByUuid() {
        UUID first;
        UUID second;
        try (DocumentDatabase db = DocumentDatabase.open(catalog, true)) {
            first = db.createDocument();
            second = db.createDocument();
            db.select(second);
            assertEquals(1L, db.reserve(first, 4));  // Deliberately leave [1, 4] unused.
            assertEquals(1L, db.reserve(second, 2)); // Same numbers in a different namespace.
        }
        try (DocumentDatabase db = DocumentDatabase.open(catalog, false)) {
            assertEquals(second, db.selected());
            assertThrows(SQLiteException.class, () -> db.reserve(UNKNOWN, 99));
            assertEquals(5L, db.reserve(first, 3));  // Leave [5, 7] unused as well.
            assertEquals(3L, db.reserve(second, 1));
        }
        try (DocumentDatabase db = DocumentDatabase.open(catalog, false)) {
            assertEquals(8L, db.reserve(first, 1));
            assertEquals(4L, db.reserve(second, 1));
        }
    }

    private static void assertOpenRejected(File path, boolean initialize) {
        assertThrows("open(" + path.getName() + ", " + initialize + ")", SQLiteException.class,
                () -> {
                    try (DocumentDatabase ignored = DocumentDatabase.open(path, initialize)) {
                        // Close an unexpectedly opened catalog before assertThrows fails.
                    }
                });
    }

    private static Path sidecar(File path, String suffix) {
        return new File(path.getPath() + suffix).toPath();
    }

    private static byte[] sentinel(String suffix) {
        return ("preserve test bytes for " + suffix).getBytes(StandardCharsets.US_ASCII);
    }

    private void assertCorruptFixtureUnchanged(byte[] corrupt) throws IOException {
        assertArrayEquals("database", corrupt, Files.readAllBytes(catalog.toPath()));
        for (String suffix : SIDECARS) {
            Path path = sidecar(catalog, suffix);
            assertTrue("Missing preserved sidecar: " + suffix, Files.exists(path));
            assertArrayEquals(suffix, sentinel(suffix), Files.readAllBytes(path));
        }
    }

    private void assertStoredViews(UUID first, long seq, double x, double y, double zoom,
                                   UUID second) {
        // Independent SQL oracle checks both the view and the separate edit revision.
        try (SQLiteDatabase db = SQLiteDatabase.openDatabase(catalog.getPath(), null,
                SQLiteDatabase.OPEN_READONLY)) {
            assertStoredView(db, first, seq, x, y, zoom);
            assertStoredView(db, second, 1, -20, 30, .5);
        }
    }

    @Test
    public void longRecordAndFaultedGroupsRecoverAtPreviousRevision() {
        final int n=41633;
        UUID id;
        try(DocumentDatabase db=DocumentDatabase.open(catalog,true)) {
            id=db.createDocument();
            assertEquals(1,db.reserve(id,10));
            db.commit(id,0,1,1,1,w->w.insert(1,0,0,new byte[80],n,
                    i->new byte[Math.min(n*36-i*DocumentDatabase.CHUNK_BYTES,DocumentDatabase.CHUNK_BYTES)]));
            assertThrows(IllegalArgumentException.class,()->db.commit(id,1,2,2,1,w->{
                w.remove(1);
                w.insert(2,0,0,new byte[80],1,i->new byte[35]);
            }));
            assertThrows(SQLiteException.class,()->db.commit(id,1,1,1,1,w->{
                // Catching a failed operation cannot turn a partial change into a commit.
                try {w.insert(2,1,1,new byte[80],1,i->new byte[36]);}
                catch(SQLiteException ignored) { }
            }));
            assertThrows(IllegalStateException.class,()->db.commit(id,1,0,0,0,w->{
                w.clear();
                db.commit(id,1,0,0,0,ignored->{});
            }));
            assertThrows(SQLiteException.class,()->db.commit(id,1,0,0,0,w->{}));
        }
        try(DocumentDatabase db=DocumentDatabase.open(catalog,false)) {
            int[] read={0,0};
            db.load(id,1,n*36,new DocumentDatabase.Restore() {
                public void begin(UUID doc,long revision,long ceiling,long count) {
                    assertEquals(id,doc); assertEquals(1,revision); assertEquals(10,ceiling); assertEquals(1,count);
                }
                public void stroke(long stroke,long prev,long next,long content,long geometry,int count,byte[] h) {
                    assertEquals(1,stroke); assertEquals(0,prev); assertEquals(0,next); assertEquals(n,count);
                }
                public void chunk(byte[] b) { assertTrue(b.length<=DocumentDatabase.CHUNK_BYTES); read[0]+=b.length; }
                public void endStroke() {read[1]++;}
            });
            assertArrayEquals(new int[]{n*36,1},read);
        }
    }

    private static void assertStoredView(SQLiteDatabase db, UUID id, long seq, double x, double y,
                                         double zoom) {
        try (Cursor c = db.rawQuery("SELECT view_seq,pan_x,pan_y,zoom,revision FROM document " +
                "WHERE uuid=?", new String[]{id.toString()})) {
            assertTrue("Missing view row for " + id, c.moveToFirst());
            assertEquals("view sequence", seq, c.getLong(0));
            assertEquals("pan x", x, c.getDouble(1), 0.0);
            assertEquals("pan y", y, c.getDouble(2), 0.0);
            assertEquals("zoom", zoom, c.getDouble(3), 0.0);
            assertEquals("Edit revision must not change for a view write", 0L, c.getLong(4));
            assertFalse(c.moveToNext());
        }
    }
}
