package org.slate.editor;

import android.content.Context;
import android.database.Cursor;
import android.database.sqlite.*;
import android.net.Uri;

// The URI and label are interned once per tab. A visit is three integers plus
// its row ID. SQLite pages the history on disk, including after an app restart.
final class Navigation extends SQLiteOpenHelper {
    record Place(long visit, long tab, Uri uri, String name, long offset, boolean preview) {}
    private long cursor;

    Navigation(Context context) {
        this(context, "navigation.db");
    }

    Navigation(Context context, String database) {
        super(context, database, null, 2);
        cursor = scalar("SELECT visit FROM position");
    }

    @Override public void onCreate(SQLiteDatabase db) {
        db.execSQL("CREATE TABLE tabs(id INTEGER PRIMARY KEY, uri TEXT NOT NULL UNIQUE, name TEXT NOT NULL, offset INTEGER NOT NULL, preview INTEGER NOT NULL)");
        db.execSQL("CREATE TABLE visits(id INTEGER PRIMARY KEY, tab INTEGER NOT NULL, offset INTEGER NOT NULL, preview INTEGER NOT NULL)");
        db.execSQL("CREATE TABLE position(visit INTEGER NOT NULL)");
        db.execSQL("INSERT INTO position VALUES(0)");
        addTabOrder(db);
    }

    @Override public void onOpen(SQLiteDatabase db) {
        db.execSQL("CREATE TEMP TABLE IF NOT EXISTS unavailable(tab INTEGER PRIMARY KEY)");
    }

    @Override public void onUpgrade(SQLiteDatabase db, int oldVersion, int newVersion) {
        if (oldVersion == 1 && newVersion == 2) addTabOrder(db);
        else throw new IllegalStateException("Unknown navigation database version");
    }

    private void addTabOrder(SQLiteDatabase db) {
        db.execSQL("CREATE TABLE tab_order(slot INTEGER PRIMARY KEY, tab INTEGER NOT NULL UNIQUE, document TEXT NOT NULL)");
        db.execSQL("CREATE INDEX tab_document ON tab_order(document)");
        db.execSQL("CREATE INDEX visit_tab ON visits(tab)");
        try (Cursor c = db.rawQuery("SELECT id,uri FROM tabs ORDER BY id", null)) {
            while (c.moveToNext()) db.execSQL("INSERT INTO tab_order VALUES(?,?,?)",
                new Object[]{c.getLong(0), c.getLong(0), Workspace.documentKey(Uri.parse(c.getString(1)))});
        }
    }

    private long scalar(String sql, String... args) {
        try (Cursor c = getReadableDatabase().rawQuery(sql, args)) {
            return c.moveToFirst() ? c.getLong(0) : 0;
        }
    }

    private Place select(String sql, long id) {
        try (Cursor c = getReadableDatabase().rawQuery(sql, new String[]{Long.toString(id)})) {
            if (!c.moveToFirst()) return null;
            return new Place(c.getLong(0), c.getLong(1), Uri.parse(c.getString(2)), c.getString(3),
                c.getLong(4), c.getInt(5) == 1);
        }
    }

    Place tab(long id) {
        return select("SELECT 0,id,uri,name,offset,preview FROM tabs WHERE id=?", id);
    }

    Place tabAt(int index) {
        return select("SELECT 0,t.id,t.uri,t.name,t.offset,t.preview FROM tabs t JOIN tab_order o ON t.id=o.tab WHERE o.slot=?", (long)index + 1);
    }

    int indexOf(long tab) {
        return (int)(scalar("SELECT slot FROM tab_order WHERE tab=?", Long.toString(tab)) - 1);
    }

    Place relative(int steps) {
        return from(cursor, steps);
    }

    Place from(long visit, int direction) {
        String condition = direction < 0 ? "v.id<? ORDER BY v.id DESC"
            : direction > 0 ? "v.id>? ORDER BY v.id" : "v.id=?";
        return select("SELECT v.id,t.id,t.uri,t.name,v.offset,v.preview FROM visits v JOIN tabs t ON t.id=v.tab "
            + "WHERE v.tab NOT IN (SELECT tab FROM unavailable) AND " + condition + " LIMIT 1", visit);
    }

    void unavailable(long tab) {
        getWritableDatabase().execSQL("INSERT OR IGNORE INTO unavailable(tab) VALUES(?)", new Object[]{tab});
    }

    int tabCount() { return (int)Math.min(Integer.MAX_VALUE, scalar("SELECT MAX(slot) FROM tab_order")); }

    void remember(long offset, boolean preview) {
        SQLiteDatabase db = getWritableDatabase();
        db.beginTransaction();
        try {
            db.execSQL("UPDATE visits SET offset=?,preview=? WHERE id=?", new Object[]{offset, preview ? 1 : 0, cursor});
            db.execSQL("UPDATE tabs SET offset=?,preview=? WHERE id=(SELECT tab FROM visits WHERE id=?)",
                new Object[]{offset, preview ? 1 : 0, cursor});
            db.setTransactionSuccessful();
        } finally { db.endTransaction(); }
    }

    void activate(long id) {
        getWritableDatabase().execSQL("UPDATE position SET visit=?", new Object[]{id});
        cursor = id;
    }

    void visit(Uri uri, String name, long offset, boolean preview) {
        SQLiteDatabase db = getWritableDatabase();
        db.beginTransaction();
        try {
            Place here = relative(0);
            if (here != null && here.uri.equals(uri)) {
                remember(offset, preview);
            } else {
                Object[] args = {uri.toString(), name, offset, preview ? 1 : 0};
                db.execSQL("INSERT OR IGNORE INTO tabs(uri,name,offset,preview) VALUES(?,?,?,?)", args);
                db.execSQL("UPDATE tabs SET name=?,offset=?,preview=? WHERE uri=?", new Object[]{name, offset, preview ? 1 : 0, uri.toString()});
                long tab = scalar("SELECT id FROM tabs WHERE uri=?", uri.toString());
                db.execSQL("INSERT OR IGNORE INTO tab_order(slot,tab,document) SELECT COALESCE(MAX(slot),0)+1,?,? FROM tab_order",
                    new Object[]{tab, Workspace.documentKey(uri)});
                db.execSQL("DELETE FROM unavailable WHERE tab=?", new Object[]{tab});
                db.execSQL("DELETE FROM visits WHERE id>?", new Object[]{cursor});
                db.execSQL("INSERT INTO visits(id,tab,offset,preview) VALUES(?,?,?,?)",
                    new Object[]{cursor + 1, tab, offset, preview ? 1 : 0});
                activate(cursor + 1);
            }
            db.setTransactionSuccessful();
        } finally { db.endTransaction(); }
    }
    // Keep interned tab IDs stable. Compact only the small tab ordering table,
    // while indexed visit removal touches just this document's history.
    void remove(Uri uri) {
        String key = Workspace.documentKey(uri);
        SQLiteDatabase db = getWritableDatabase();
        db.beginTransaction();
        try {
            while (true) {
                long slot = scalar("SELECT MAX(slot) FROM tab_order WHERE document=?", key);
                if (slot == 0) break;
                long tab = scalar("SELECT tab FROM tab_order WHERE slot=?", Long.toString(slot));
                String[] id = {Long.toString(tab)};
                db.delete("visits", "tab=?", id);
                db.delete("tabs", "id=?", id);
                db.delete("unavailable", "tab=?", id);
                db.delete("tab_order", "tab=?", id);
                db.execSQL("UPDATE tab_order SET slot=-slot WHERE slot>?", new Object[]{slot});
                db.execSQL("UPDATE tab_order SET slot=-slot-1 WHERE slot<0");
            }
            if (relative(0) == null) {
                Place before = relative(-1);
                Place nearest = before != null ? before : relative(1);
                activate(nearest == null ? 0 : nearest.visit);
            }
            db.setTransactionSuccessful();
        } finally { db.endTransaction(); }
    }

}
