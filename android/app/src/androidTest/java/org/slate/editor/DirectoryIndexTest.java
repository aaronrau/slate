package org.slate.editor;

import android.content.*;
import android.content.pm.*;
import android.database.*;
import android.net.Uri;
import android.os.*;
import android.provider.DocumentsContract;
import android.test.InstrumentationTestCase;
import java.io.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

@SuppressWarnings("deprecation")
public final class DirectoryIndexTest extends InstrumentationTestCase {
    private static final int LARGE = 178133;
    private static final String ROOT = "primary:Documents/Work Bench Audio";
    private static final Uri TREE = DocumentsContract.buildTreeDocumentUri(Workspace.EXTERNAL,ROOT);

    // A lazy cursor lets the test hold the provider after the first page, count
    // enumerations and reject a regression that relies on provider-side pagination.
    private static final class Source extends ContentProvider {
        final AtomicInteger queries = new AtomicInteger(), moves = new AtomicInteger(), opens = new AtomicInteger();
        CountDownLatch release = new CountDownLatch(1);
        int size = LARGE;
        boolean audio;
        File text;
        @Override public boolean onCreate() { return true; }
        String name(int pos) {
            if (pos == size-1) return "one needle Σ take.txt";
            if (audio) return "sound "+pos+(pos%2 == 0 ? ".wav (2)" : ".audio");
            return "Workbench take " + pos + (pos%3 == 0 ? ".wav" : ".txt");
        }
        @Override public Cursor query(Uri uri,String[] columns,String selection,String[] args,String order) {
            queries.incrementAndGet();
            return new AbstractCursor() {
                @Override public int getCount() { return size; }
                @Override public boolean onMove(int oldPos,int newPos) {
                    moves.incrementAndGet();
                    if (newPos == 129) try {
                        if (!release.await(20,TimeUnit.SECONDS)) throw new AssertionError("First page waited for enumeration");
                    } catch (InterruptedException e) { throw new AssertionError(e); }
                    return true;
                }
                @Override public String[] getColumnNames() { return columns; }
                @Override public String getString(int col) {
                    String name = name(getPosition());
                    return switch (col) {
                        case 0 -> ROOT + "/" + name;
                        case 1 -> name;
                        default -> name.endsWith(".txt") ? "text/plain" : name.endsWith(".audio") ? "audio/wav" : "application/octet-stream";
                    };
                }
                @Override public boolean isNull(int col) { return false; }
                @Override public short getShort(int col) { throw new UnsupportedOperationException(); }
                @Override public int getInt(int col) { throw new UnsupportedOperationException(); }
                @Override public long getLong(int col) { throw new UnsupportedOperationException(); }
                @Override public float getFloat(int col) { throw new UnsupportedOperationException(); }
                @Override public double getDouble(int col) { throw new UnsupportedOperationException(); }
            };
        }
        @Override public ParcelFileDescriptor openFile(Uri uri,String mode) throws FileNotFoundException {
            opens.incrementAndGet();
            if (!DocumentsContract.getDocumentId(uri).endsWith(".txt")) throw new AssertionError("Opened audio: " + uri);
            return ParcelFileDescriptor.open(text,ParcelFileDescriptor.MODE_READ_ONLY);
        }
        @Override public String getType(Uri uri) { return "text/plain"; }
        @Override public Uri insert(Uri uri,ContentValues v) { throw new UnsupportedOperationException(); }
        @Override public int delete(Uri uri,String s,String[] a) { throw new UnsupportedOperationException(); }
        @Override public int update(Uri uri,ContentValues v,String s,String[] a) { throw new UnsupportedOperationException(); }
    }

    public void testPublishedPrefixPersistentPagesNameSearchAndAudioFiltering() throws Exception {
        if (Build.VERSION.SDK_INT < 29) return;
        Context target = getInstrumentation().getTargetContext();
        final boolean[] grant = {true};
        Context context = new ContextWrapper(target) {
            @Override public int checkUriPermission(Uri uri,int pid,int uid,int flags) {
                return grant[0] ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
            }
        };
        Source source = new Source();
        ProviderInfo info = new ProviderInfo();
        info.authority = Workspace.EXTERNAL;
        source.attachInfo(context,info);
        ContentResolver resolver = ContentResolver.wrap(source);
        String database = "directory-test-" + System.nanoTime() + ".db";
        DirectoryIndex index = new DirectoryIndex(context,database);
        source.text = File.createTempFile("slate-content-", ".txt",context.getCacheDir());
        try {
            try (FileOutputStream out = new FileOutputStream(source.text)) { out.write("before FIND after".getBytes("UTF-8")); }
            Workspace ws = new Workspace(resolver,TREE,index);
            CancellationSignal cancel = new CancellationSignal();
            long start = SystemClock.elapsedRealtime();
            assertEquals(128, ws.list(ws.root,0,cancel).entries().size());
            long first = SystemClock.elapsedRealtime()-start;
            assertTrue(source.moves.get() < 140);
            source.release.countDown();
            assertEquals(LARGE, index.window(ws,ws.root,Integer.MAX_VALUE,true,cancel).count());
            long build = SystemClock.elapsedRealtime()-start;
            assertEquals(1, source.queries.get());
            assertEquals(LARGE, source.moves.get());
            index.close();
            long bytes = context.getDatabasePath(database).length();
            index = new DirectoryIndex(context,database);
            ws = new Workspace(resolver,TREE,index);
            long warm = SystemClock.elapsedRealtime();
            assertEquals(128,ws.list(ws.root,100000,cancel).entries().size());
            warm = SystemClock.elapsedRealtime()-warm;
            long search = SystemClock.elapsedRealtime();
            Workspace.Results matches = Workspace.find(resolver,Collections.singletonList(new Workspace.Scope(ws,"Audio")),
                Collections.emptyList(),"eDlE σ",false,cancel,null);
            search = SystemClock.elapsedRealtime()-search;
            assertEquals(1,matches.entries().size());
            assertEquals(source.name(LARGE-1),matches.entries().get(0).name());
            assertEquals(1,source.queries.get());
            assertEquals(0,source.opens.get());
            android.util.Log.i("SlateIndexTest","rows="+LARGE+" first_ms="+first+" build_ms="+build+" warm_ms="+warm+
                " name_ms="+search+" file_bytes="+bytes+" provider_queries="+source.queries.get());
            source.audio = true;
            source.size = 3000;
            source.release = new CountDownLatch(1);
            CountDownLatch refreshed = new CountDownLatch(1);
            index.listeners.add((grantUri,parent,success) -> { if (success) refreshed.countDown(); });
            index.refresh();
            assertTrue(ws.list(ws.root,100000,cancel).entries().get(0).name().startsWith("Workbench take "));
            source.release.countDown();
            assertTrue("replacement snapshot did not finish",refreshed.await(15,TimeUnit.SECONDS));
            List<List<Workspace.Entry>> published = new ArrayList<>();
            matches = Workspace.find(resolver,Collections.singletonList(new Workspace.Scope(ws,"Audio")),Collections.emptyList(),
                "FIND",true,cancel,published::add);
            assertEquals(1,source.opens.get());
            assertEquals(1,published.size());
            assertEquals(7,matches.entries().get(0).offset());
            assertEquals(2,source.queries.get());
            grant[0] = false;
            try { ws.list(ws.root,0,cancel); fail("Revoked folder was visible"); }
            catch (SecurityException expected) {}
            index.pruneRevoked();
            grant[0] = true;
            ws.list(ws.root,0,cancel);
            assertEquals(3,source.queries.get());
            index.window(ws,ws.root,Integer.MAX_VALUE,true,cancel);
        } finally {
            source.release.countDown();
            index.close();
            context.deleteDatabase(database);
            source.text.delete();
        }
    }
}
