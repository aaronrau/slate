package org.slate.editor;

import android.content.*;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.database.sqlite.*;
import android.net.Uri;
import android.os.CancellationSignal;
import android.os.OperationCanceledException;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;

import java.io.IOException;
import java.util.*;
import java.util.concurrent.*;

// One provider enumeration per directory. Only metadata enters the private database.
// Readers can use a committed prefix while the rest of a large cursor is still arriving.
final class DirectoryIndex extends SQLiteOpenHelper {
    record Row(String id, String name, String mime) {
        boolean directory() { return Document.MIME_TYPE_DIR.equals(mime); }
    }
    record Window(long id, int count, boolean complete, boolean stale) {}
    interface Listener { void replaced(Uri tree, String parent, boolean success); }
    final Set<Listener> listeners = new CopyOnWriteArraySet<>();
    private record Key(String tree, String parent) {}
    private static final class Fill {
        final long id;
        final CancellationSignal stop = new CancellationSignal();
        int count, waiters;
        boolean browse, done;
        Exception error;
        final Window previous;
        Fill(long id, Window previous) { this.id = id; this.previous = previous; }
    }

    private static DirectoryIndex instance;
    static synchronized DirectoryIndex get(Context context) {
        if (instance == null) instance = new DirectoryIndex(context.getApplicationContext(), "directory-index.db");
        return instance;
    }

    private final Context context;
    private final Object gate = new Object();
    private final Map<Key, Fill> active = new HashMap<>();
    private final ExecutorService readers = Executors.newFixedThreadPool(2);
    private final ExecutorService janitor = Executors.newSingleThreadExecutor();
    private boolean cleaning;

    DirectoryIndex(Context context, String database) {
        super(context, database, null, 3);
        this.context = context;
        setWriteAheadLoggingEnabled(true);
    }

    @Override public void onCreate(SQLiteDatabase db) {
        db.execSQL("CREATE TABLE folders(id INTEGER PRIMARY KEY AUTOINCREMENT, tree TEXT, parent TEXT, n INTEGER DEFAULT 0, done INTEGER DEFAULT 0, stale INTEGER DEFAULT 0, building INTEGER DEFAULT 0, UNIQUE(tree,parent))");
        db.execSQL("CREATE TABLE entries(folder INTEGER,pos INTEGER,doc TEXT,name TEXT,folded TEXT,kind INTEGER,PRIMARY KEY(folder,pos)) WITHOUT ROWID");
        db.execSQL("CREATE INDEX by_document ON entries(doc,folder) WHERE doc IS NOT NULL");
        db.execSQL("CREATE INDEX by_kind ON entries(folder,kind,pos)");
        db.execSQL("CREATE VIRTUAL TABLE grams USING fts4(tokens, tokenize=simple, matchinfo=fts3)");
    }
    @Override public void onUpgrade(SQLiteDatabase db, int oldVersion, int newVersion) {
        db.execSQL("DROP TABLE grams");
        db.execSQL("DROP TABLE entries");
        db.execSQL("DROP TABLE folders");
        onCreate(db);
    }

    @Override public void onOpen(SQLiteDatabase db) {
        super.onOpen(db);
        // Another process lifetime may have ended during a replacement.
        db.execSQL("UPDATE folders SET building=0 WHERE tree IS NULL AND building<>0");
    }

    void checkAccess(Uri tree) {
        if (context.checkUriPermission(tree, android.os.Process.myPid(), android.os.Process.myUid(),
            Intent.FLAG_GRANT_READ_URI_PERMISSION) != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("Folder access was removed. Open the folder to grant access again.");
        }
    }

    private Window state(SQLiteDatabase db, Key key) {
        try (Cursor c = db.rawQuery("SELECT id,n,done,stale FROM folders WHERE tree=? AND parent=?",
            new String[]{key.tree,key.parent})) {
            return c.moveToFirst() ? new Window(c.getLong(0), c.getInt(1), c.getInt(2) != 0,c.getInt(3) != 0) : null;
        }
    }

    Window window(Workspace ws, Uri parent, int need, boolean browse, CancellationSignal cancel) throws IOException {
        cancel.throwIfCanceled();
        Key key = new Key(ws.tree.toString(), DocumentsContract.getDocumentId(parent));
        Fill fill;
        synchronized (gate) {
            SQLiteDatabase db = getWritableDatabase();
            fill = active.get(key);
            if (fill == null) {
                Window saved = state(db, key);
                if (saved != null && !saved.stale && (saved.complete || saved.count >= need)) return saved;
                // A previous process died partway through its cursor. Its prefix remains
                // usable until a request crosses it. Never append another provider snapshot.
                Window previous = saved != null && saved.complete ? saved : null;
                if (saved != null && previous == null) forget(db, saved.id);
                ContentValues values = new ContentValues();
                if (previous == null) values.put("tree", key.tree);
                else values.put("building",1);
                values.put("parent", key.parent);
                fill = new Fill(db.insertOrThrow("folders", null, values), previous);
                active.put(key, fill);
                Fill work = fill;
                readers.execute(() -> populate(ws, parent, key, work));
            }
            if (fill.previous != null) return fill.previous;
            fill.waiters++;
            fill.browse |= browse;
            try {
                while (!fill.done && fill.count < need && !fill.stop.isCanceled()) {
                    cancel.throwIfCanceled();
                    try { gate.wait(70); }
                    catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new OperationCanceledException(); }
                }
                cancel.throwIfCanceled();
                if (fill.error != null) throw new IOException("Could not index this folder", fill.error);
                fill.stop.throwIfCanceled();
                return new Window(fill.id, fill.count, fill.done,false);
            } finally {
                fill.waiters--;
                if (cancel.isCanceled() && !fill.browse && fill.waiters == 0 && !fill.done) fill.stop.cancel();
            }
        }
    }

    private void populate(Workspace ws, Uri parent, Key key, Fill fill) {
        SQLiteDatabase db = getWritableDatabase();
        try (Cursor c = ws.children(parent, fill.stop);
             SQLiteStatement row = db.compileStatement("INSERT INTO entries(folder,pos,doc,name,folded,kind) VALUES(?,?,?,?,?,?)");
             SQLiteStatement names = db.compileStatement("INSERT INTO grams(docid,tokens) VALUES(?,?)")) {
            if (c.getExtras().getBoolean(DocumentsContract.EXTRA_LOADING))
                throw new IOException("The provider is still loading this folder. Refresh to try again.");
            int total = 0;
            ArrayList<Row> batch = new ArrayList<>(512);
            String prefix = Workspace.EXTERNAL.equals(ws.tree.getAuthority()) ? childPrefix(key.parent) : null;
            BitSet tokens = new BitSet(36 * 36 * 36);
            boolean current = c.moveToNext();
            do {
                batch.clear();
                int limit = total == 0 ? Workspace.PAGE : 512;
                while (current && batch.size() < limit) {
                    fill.stop.throwIfCanceled();
                    String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                    if (id != null) batch.add(new Row(id, name == null ? id : name, mime == null ? "" : mime));
                    current = c.moveToNext();
                }
                boolean end = !current;
                db.beginTransactionNonExclusive();
                try {
                    for (Row r : batch) {
                        String folded = r.name.toLowerCase(Locale.ROOT);
                        row.bindLong(1, fill.id); row.bindLong(2, total);
                        if (prefix != null && r.id.equals(prefix + r.name)) row.bindNull(3);
                        else row.bindString(3, r.id);
                        row.bindString(4, r.name);
                        if (folded.equals(r.name)) row.bindNull(5); else row.bindString(5, folded);
                        row.bindLong(6, r.directory() ? 1 : Workspace.textCandidate(r.name, r.mime) ? 2 : 0);
                        row.execute();
                        if (!r.directory()) addTokens(tokens, folded);
                        if (++total % Workspace.PAGE == 0) saveTokens(names,fill.id,total-1,tokens);
                    }
                    if (end && total % Workspace.PAGE != 0) saveTokens(names,fill.id,total-1,tokens);
                    fill.stop.throwIfCanceled();
                    db.execSQL("UPDATE folders SET n=?,done=? WHERE id=?", new Object[]{total, end ? 1 : 0, fill.id});
                    db.setTransactionSuccessful();
                } finally { db.endTransaction(); }
                synchronized (gate) {
                    if (end && fill.previous != null) {
                        fill.stop.throwIfCanceled();
                        db.beginTransactionNonExclusive();
                        try {
                            db.execSQL("UPDATE folders SET tree=NULL WHERE id=?",new Object[]{fill.previous.id});
                            db.execSQL("UPDATE folders SET tree=?,building=0 WHERE id=?",new Object[]{key.tree,fill.id});
                            db.setTransactionSuccessful();
                        } finally { db.endTransaction(); }
                        scheduleCleanup();
                    }
                    fill.count = total; fill.done = end;
                    gate.notifyAll();
                }
            } while (current);
        } catch (Exception ex) {
            synchronized (gate) {
                fill.error = ex;
                // An unsupported or revoked provider cannot leave a falsely complete index.
                forget(db, fill.id);
                if (fill.previous != null) db.execSQL("UPDATE folders SET stale=0 WHERE id=?",new Object[]{fill.previous.id});
            }
        } finally {
            synchronized (gate) {
                boolean current = active.remove(key, fill);
                if (current && fill.previous != null)
                    for (Listener listener : listeners) listener.replaced(ws.tree,key.parent,fill.error == null);
                fill.done = true;
                gate.notifyAll();
            }
        }
    }

    // ASCII tokens index overlapping substrings with FTS4 on every supported Android.
    // Mapping unusual characters into the same alphabet only adds candidates. instr()
    // subsequently verifies the original, case-folded string, including punctuation.
    private static int letter(char c) {
        return c >= 'a' && c <= 'z' ? c-'a'+10 : c >= '0' && c <= '9' ? c-'0' : c % 36;
    }

    private static void addTokens(BitSet bits,String name) {
        for (int i = 2; i < name.length(); i++)
            bits.set((letter(name.charAt(i-2)) * 36 + letter(name.charAt(i-1))) * 36 + letter(name.charAt(i)));
    }

    private static String tokens(BitSet bits) {
        StringBuilder out = new StringBuilder();
        for (int i = bits.nextSetBit(0); i >= 0; i = bits.nextSetBit(i+1)) {
            if (out.length() > 0) out.append(' ');
            out.append(Character.forDigit(i / 1296, 36)).append(Character.forDigit(i / 36 % 36, 36))
                .append(Character.forDigit(i % 36, 36));
        }
        return out.toString();
    }

    // A posting identifies a 128-row block. Similar recording names share
    // most trigrams. Each distinct trigram is written just once per block.
    private static void saveTokens(SQLiteStatement names,long folder,int pos,BitSet bits) {
        if (bits.isEmpty()) return;
        names.bindLong(1, blockId(folder,pos));
        names.bindString(2,tokens(bits));
        names.executeInsert();
        bits.clear();
    }

    private static long blockId(long folder,int pos) { return (folder << 32) + pos / Workspace.PAGE; }

    static String childPrefix(String parent) { return parent.endsWith(":") ? parent : parent + "/"; }

    private static Row decode(Cursor c,int column,String prefix) {
        String name = c.getString(column+1);
        String id = c.isNull(column) ? prefix + name : c.getString(column);
        return new Row(id,name,c.getInt(column+2) == 1 ? Document.MIME_TYPE_DIR : "");
    }

    List<Row> rows(long folder, Uri parent, int first, int end, int limit, CancellationSignal cancel) {
        ArrayList<Row> rows = new ArrayList<>();
        String prefix = childPrefix(DocumentsContract.getDocumentId(parent));
        try (Cursor c = getReadableDatabase().rawQuery("SELECT doc,name,kind FROM entries WHERE folder=? AND pos>=? AND pos<? ORDER BY pos LIMIT ?",
            args(folder,first,end,limit),cancel)) {
            while (c.moveToNext()) rows.add(decode(c,0,prefix));
        }
        return rows;
    }

    interface Visitor { boolean visit(Row row) throws IOException; }

    // Visit only directories (1), eligible text files (2), or filename matches (3).
    // Release the SQLite cursor before calling out to another directory or file stream.
    void visit(Workspace ws, Uri parent, int kind, String needle, CancellationSignal cancel, Visitor visitor) throws IOException {
        int first = 0;
        String prefix = childPrefix(DocumentsContract.getDocumentId(parent));
        BitSet query = new BitSet();
        if (kind == 3 && needle.length() >= 3) addTokens(query,needle.substring(0,Math.min(needle.length(),24)));
        String match = tokens(query);
        while (true) {
            Window page = window(ws, parent, first + 1, false, cancel);
            int end = page.count;
            while (first < end) {
                cancel.throwIfCanceled();
                ArrayList<Row> batch = new ArrayList<>(Workspace.PAGE);
                ArrayList<String> values = new ArrayList<>();
                String sql;
                if (!match.isEmpty()) {
                    // Fetch only candidate blocks in key order. LIMIT applies before reading
                    // more postings; common search terms can report their first hits quickly.
                    long id;
                    try (Cursor c = getReadableDatabase().rawQuery(
                        "SELECT docid FROM grams WHERE grams MATCH ? AND docid>=? AND docid<=? ORDER BY docid LIMIT 1",
                        new String[]{match,Long.toString(blockId(page.id,first)),Long.toString(blockId(page.id,end-1))},cancel)) {
                        if (!c.moveToFirst()) { first = end; break; }
                        id = c.getLong(0);
                    }
                    int begin = (int)id * Workspace.PAGE;
                    first = Math.max(first,begin);
                    sql = "SELECT pos,doc,name,kind FROM entries WHERE kind<>1 AND instr(coalesce(folded,name),?)>0";
                    values.add(needle);
                    values.addAll(Arrays.asList(args(page.id,first,Math.min(end,begin + Workspace.PAGE),Workspace.PAGE)));
                    first = Math.min(end,begin + Workspace.PAGE);
                } else {
                    sql = kind == 3 ? "SELECT pos,doc,name,kind FROM entries WHERE kind<>1 AND instr(coalesce(folded,name),?)>0"
                        : "SELECT pos,doc,name,kind FROM entries WHERE kind=?";
                    values.add(kind == 3 ? needle : Integer.toString(kind));
                    values.addAll(Arrays.asList(args(page.id,first,end,Workspace.PAGE)));
                }
                sql += " AND folder=? AND pos>=? AND pos<? ORDER BY pos LIMIT ?";
                try (Cursor c = getReadableDatabase().rawQuery(sql,values.toArray(new String[0]),cancel)) {
                    while (c.moveToNext()) {
                        if (match.isEmpty()) first = c.getInt(0)+1;
                        batch.add(decode(c,1,prefix));
                    }
                }
                for (Row r : batch) {
                    cancel.throwIfCanceled();
                    if (!visitor.visit(r)) return;
                }
                if (match.isEmpty() && batch.size() < Workspace.PAGE) first = end;
            }
            if (page.complete) return;
        }
    }

    int position(Workspace ws, Uri parent, String document, CancellationSignal cancel) throws IOException {
        int count = 0;
        String prefix = childPrefix(DocumentsContract.getDocumentId(parent));
        boolean relative = Workspace.EXTERNAL.equals(parent.getAuthority()) && document.startsWith(prefix);
        String name = relative ? document.substring(prefix.length()) : null;
        while (true) {
            Window w = window(ws, parent, count + 1, true, cancel);
            String where = relative ? "((doc IS NULL AND name=?) OR doc=?)" : "doc=?";
            ArrayList<String> values = new ArrayList<>();
            if (relative) values.add(name);
            values.add(document);
            values.addAll(Arrays.asList(args(w.id,count,w.count)));
            try (Cursor c = getReadableDatabase().rawQuery("SELECT pos FROM entries WHERE " + where +
                " AND folder=? AND pos>=? AND pos<? LIMIT 1",values.toArray(new String[0]),cancel)) {
                if (c.moveToFirst()) return c.getInt(0);
            }
            if (w.complete) return -1;
            count = w.count;
        }
    }

    private static String[] args(long... numbers) {
        String[] values = new String[numbers.length];
        for (int i = 0; i < numbers.length; i++) values[i] = Long.toString(numbers[i]);
        return values;
    }

    // Detaching is cheap even for a 200,000-entry directory. Reclaim its rows in
    // small transactions so a refresh/new grant can display its first page promptly.
    private void forget(SQLiteDatabase db, long id) {
        db.execSQL("UPDATE folders SET tree=NULL,building=0 WHERE id=?",new Object[]{id});
        scheduleCleanup();
    }

    private void scheduleCleanup() {
        if (cleaning) return;
        cleaning = true;
        janitor.execute(() -> {
            try {
                SQLiteDatabase db = getWritableDatabase();
                while (true) {
                    long id;
                    synchronized (gate) {
                        try (Cursor orphan = db.rawQuery("SELECT id FROM folders WHERE tree IS NULL AND building=0 LIMIT 1",null)) {
                            if (!orphan.moveToFirst()) { cleaning = false; return; }
                            id = orphan.getLong(0);
                        }
                    }
                    int deleted = db.delete("entries","folder=? AND pos IN(SELECT pos FROM entries WHERE folder=? LIMIT 1024)",args(id,id));
                    if (deleted == 0) {
                        db.delete("grams","docid>=? AND docid<?",args(id<<32,(id+1)<<32));
                        db.delete("folders","id=?",args(id));
                    }
                }
            } catch (RuntimeException e) {
                synchronized (gate) { cleaning = false; }
                android.util.Log.w("Slate", "Could not reclaim directory cache", e);
            }
        });
    }

    private void invalidate(Key key) {
        synchronized (gate) {
            SQLiteDatabase db = getWritableDatabase();
            Fill work = active.remove(key);
            if (work != null) work.stop.cancel();
            Window w = state(db,key);
            if (w != null) forget(db,w.id);
            gate.notifyAll();
        }
    }

    void invalidateParent(Uri tree, Uri parent) {
        String doc = DocumentsContract.getDocumentId(parent);
        for (Key key : keys("parent=?",new String[]{doc})) {
            if (Objects.equals(Uri.parse(key.tree).getAuthority(),tree.getAuthority())) invalidate(key);
        }
    }

    void created(Uri tree,Uri parent,Uri file,Workspace.Info info) {
        String parentId = DocumentsContract.getDocumentId(parent), id = DocumentsContract.getDocumentId(file);
        synchronized (gate) {
            SQLiteDatabase db = getWritableDatabase();
            for (Key key : keys("parent=?",new String[]{parentId})) {
                if (!Objects.equals(Uri.parse(key.tree).getAuthority(),tree.getAuthority())) continue;
                Window w = state(db,key);
                if (w == null || !w.complete || w.stale || active.containsKey(key)) { invalidate(key); continue; }
                db.beginTransactionNonExclusive();
                try {
                    String name = info.name(), folded = name.toLowerCase(Locale.ROOT);
                    boolean relative = Workspace.EXTERNAL.equals(file.getAuthority()) && id.equals(childPrefix(parentId)+name);
                    db.execSQL("INSERT INTO entries(folder,pos,doc,name,folded,kind) VALUES(?,?,?,?,?,?)",
                        new Object[]{w.id,w.count,relative ? null : id,name,name.equals(folded) ? null : folded,
                            Workspace.textCandidate(name,info.mime()) ? 2 : 0});
                    rebuildBlock(db,w.id,w.count);
                    db.execSQL("UPDATE folders SET n=n+1 WHERE id=?",new Object[]{w.id});
                    db.setTransactionSuccessful();
                } finally { db.endTransaction(); }
            }
        }
    }

    private static void rebuildBlock(SQLiteDatabase db,long folder,int pos) {
        int begin = pos / Workspace.PAGE * Workspace.PAGE;
        BitSet bits = new BitSet();
        try (Cursor c = db.rawQuery("SELECT coalesce(folded,name) FROM entries WHERE folder=? AND pos>=? AND pos<? AND kind<>1",
            args(folder,begin,begin + Workspace.PAGE))) {
            while (c.moveToNext()) addTokens(bits,c.getString(0));
        }
        long id = blockId(folder,pos);
        db.delete("grams","docid=?",args(id));
        try (SQLiteStatement insert = db.compileStatement("INSERT INTO grams(docid,tokens) VALUES(?,?)")) {
            saveTokens(insert,folder,pos,bits);
        }
    }

    void deleteDocument(Uri uri) {
        List<String> parts = uri.getPathSegments();
        int i = parts.indexOf("document");
        if (i < 0 || i+1 == parts.size()) return;
        String doc = parts.get(i+1);
        boolean external = Workspace.EXTERNAL.equals(uri.getAuthority());
        String parent = external ? DocumentsContract.getDocumentId(Workspace.parentHint(context.getContentResolver(),uri)) : "";
        String name = external && doc.startsWith(childPrefix(parent)) ? doc.substring(childPrefix(parent).length()) : "";
        synchronized (gate) {
            SQLiteDatabase db = getWritableDatabase();
            List<Key> affected = keys("done=0 OR parent=? OR id IN(SELECT folder FROM entries WHERE doc=?)",new String[]{parent,doc});
            for (Key key : affected) {
                if (!Objects.equals(Uri.parse(key.tree).getAuthority(),uri.getAuthority())) continue;
                Window w = state(db,key);
                if (w == null || !w.complete || w.stale || active.containsKey(key)) { invalidate(key); continue; }
                int pos;
                try (Cursor c = db.rawQuery("SELECT pos FROM entries WHERE folder=? AND (doc=? OR (doc IS NULL AND name=?)) LIMIT 1",
                    new String[]{Long.toString(w.id),doc,name})) {
                    if (!c.moveToFirst()) continue;
                    pos = c.getInt(0);
                }
                // Keep all numbered pages dense without shifting every following entry.
                db.beginTransactionNonExclusive();
                try {
                    int last = w.count-1;
                    db.delete("entries","folder=? AND pos=?",args(w.id,pos));
                    if (pos != last) db.execSQL("UPDATE entries SET pos=? WHERE folder=? AND pos=?",new Object[]{pos,w.id,last});
                    rebuildBlock(db,w.id,pos);
                    if (pos / Workspace.PAGE != last / Workspace.PAGE) rebuildBlock(db,w.id,last);
                    db.execSQL("UPDATE folders SET n=n-1 WHERE id=?",new Object[]{w.id});
                    db.setTransactionSuccessful();
                } finally { db.endTransaction(); }
            }
        }
    }

    private List<Key> keys(String where,String[] values) {
        ArrayList<Key> keys = new ArrayList<>();
        try (Cursor c = getReadableDatabase().rawQuery("SELECT tree,parent FROM folders WHERE tree IS NOT NULL AND ("+where+")",values)) {
            while (c.moveToNext()) keys.add(new Key(c.getString(0),c.getString(1)));
        }
        return keys;
    }

    void refresh() {
        synchronized (gate) {
            for (Key k : keys("done=0",null)) invalidate(k);
            getWritableDatabase().execSQL("UPDATE folders SET stale=1 WHERE done=1 AND tree IS NOT NULL");
        }
    }

    void cancelSearch() {
        synchronized (gate) {
            for (Fill fill : active.values()) if (!fill.browse && fill.waiters == 0 && fill.previous == null)
                fill.stop.cancel();
        }
    }

    void pruneRevoked() {
        ArrayList<Uri> trees = new ArrayList<>();
        try (Cursor c = getReadableDatabase().rawQuery("SELECT DISTINCT tree FROM folders WHERE tree IS NOT NULL",null)) {
            while (c.moveToNext()) trees.add(Uri.parse(c.getString(0)));
        }
        for (Uri tree : trees) {
            try { checkAccess(tree); }
            catch (SecurityException e) {
                for (Key k : keys("tree=?",new String[]{tree.toString()})) invalidate(k);
            }
        }
        synchronized (gate) { scheduleCleanup(); }
    }
    @Override public void close() {
        synchronized (gate) { for (Fill fill : active.values()) fill.stop.cancel(); }
        readers.shutdown();
        janitor.shutdown();
        try {
            readers.awaitTermination(10,TimeUnit.SECONDS);
            janitor.awaitTermination(10,TimeUnit.SECONDS);
        } catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); }
        super.close();
    }
}
