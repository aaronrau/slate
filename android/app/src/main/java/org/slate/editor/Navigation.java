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
        super(context, database, null, 1);
        cursor = scalar("SELECT visit FROM position");
    }

    @Override public void onCreate(SQLiteDatabase db) {
        db.execSQL("CREATE TABLE tabs(id INTEGER PRIMARY KEY, uri TEXT NOT NULL UNIQUE, name TEXT NOT NULL, offset INTEGER NOT NULL, preview INTEGER NOT NULL)");
        db.execSQL("CREATE TABLE visits(id INTEGER PRIMARY KEY, tab INTEGER NOT NULL, offset INTEGER NOT NULL, preview INTEGER NOT NULL)");
        db.execSQL("CREATE TABLE position(visit INTEGER NOT NULL)");
        db.execSQL("INSERT INTO position VALUES(0)");
    }

    @Override public void onOpen(SQLiteDatabase db) {
        db.execSQL("CREATE TEMP TABLE IF NOT EXISTS unavailable(tab INTEGER PRIMARY KEY)");
    }

    @Override public void onUpgrade(SQLiteDatabase db, int oldVersion, int newVersion) {
        throw new IllegalStateException("Unknown navigation database version");
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

    int tabCount() { return (int)Math.min(Integer.MAX_VALUE, scalar("SELECT MAX(id) FROM tabs")); }

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
                db.execSQL("DELETE FROM unavailable WHERE tab=?", new Object[]{tab});
                db.execSQL("DELETE FROM visits WHERE id>?", new Object[]{cursor});
                db.execSQL("INSERT INTO visits(id,tab,offset,preview) VALUES(?,?,?,?)",
                    new Object[]{cursor + 1, tab, offset, preview ? 1 : 0});
                activate(cursor + 1);
            }
            db.setTransactionSuccessful();
        } finally { db.endTransaction(); }
    }
}
