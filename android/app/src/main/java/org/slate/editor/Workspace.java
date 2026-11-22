package org.slate.editor;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.os.CancellationSignal;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;

import java.io.*;
import java.util.*;

import static java.nio.charset.StandardCharsets.UTF_8;

final class Workspace {
    private static final String[] COLUMNS = {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME,
        Document.COLUMN_MIME_TYPE};
    static final int PAGE = 128, LIMIT = 200;
    record Entry(String name, String path, Uri uri, boolean directory, long offset, int length) {}
    record Listing(List<Entry> entries, boolean more) {}
    record Results(List<Entry> entries, int unreadable) {}
    record Scope(Workspace workspace, String name) {}

    final Uri tree;
    final Uri root;
    private final ContentResolver resolver;

    Workspace(ContentResolver resolver, Uri tree) {
        this.resolver = resolver;
        this.tree = tree;
        root = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
    }

    private Cursor children(Uri folder, CancellationSignal cancel) throws IOException {
        String id = DocumentsContract.getDocumentId(folder);
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
        Cursor cursor = resolver.query(children, COLUMNS, null, null, null, cancel);
        if (cursor == null) throw new IOException("Folder could not be read");
        return cursor;
    }

    private Entry entry(Cursor c, String parent) {
        String name = c.getString(1);
        return new Entry(name, parent + name, DocumentsContract.buildDocumentUriUsingTree(tree, c.getString(0)),
            Document.MIME_TYPE_DIR.equals(c.getString(2)), -1, 0);
    }

    Listing list(Uri folder, int skip, CancellationSignal cancel) throws IOException {
        ArrayList<Entry> entries = new ArrayList<>();
        try (Cursor c = children(folder, cancel)) {
            c.moveToPosition(skip - 1);
            while (entries.size() < PAGE && c.moveToNext()) entries.add(entry(c, ""));
            boolean more = c.moveToNext();
            entries.sort(Comparator.comparing(Entry::directory).reversed()
                .thenComparing(Entry::name, String.CASE_INSENSITIVE_ORDER));
            return new Listing(entries, more);
        }
    }

    static Results find(ContentResolver resolver, List<Scope> folders, List<Entry> files, String term,
                        boolean contents, CancellationSignal cancel) {
        Scan scan = new Scan(resolver, term, contents, cancel);
        for (Scope scope : folders) {
            if (scan.hits.size() >= LIMIT) break;
            scan.walk(scope.workspace, scope.workspace.root, folders.size() > 1 ? scope.name + "/" : "");
        }
        for (Entry file : files) {
            if (scan.hits.size() >= LIMIT) break;
            cancel.throwIfCanceled();
            scan.match(file);
        }
        return new Results(scan.hits, scan.skipped);
    }

    private static class Scan {
        final ContentResolver resolver;
        final String query;
        final byte[] term;
        final int[] failure;
        final byte[] block = new byte[64 * 1024];
        final CancellationSignal cancel;
        final boolean contents;
        final List<Entry> hits = new ArrayList<>();
        final Set<Uri> ancestors = new HashSet<>();
        record Match(String authority, String document, long offset) {}
        final Set<Match> found = new HashSet<>();
        int skipped;

        Scan(ContentResolver resolver, String query, boolean contents, CancellationSignal cancel) {
            this.resolver = resolver;
            this.query = query.toLowerCase(Locale.ROOT);
            term = query.getBytes(UTF_8);
            this.contents = contents;
            this.cancel = cancel;
            failure = new int[term.length];
            for (int i = 1, j = 0; i < term.length; i++) {
                while (j > 0 && term[i] != term[j]) j = failure[j - 1];
                if (term[i] == term[j]) failure[i] = ++j;
            }
        }

        void walk(Workspace ws, Uri dir, String prefix) {
            if (ancestors.size() >= 128 || !ancestors.add(dir)) { skipped++; return; }
            try (Cursor cursor = ws.children(dir, cancel)) {
                while (hits.size() < LIMIT && cursor.moveToNext()) {
                    cancel.throwIfCanceled();
                    Entry e = ws.entry(cursor, prefix);
                    if (e.directory) walk(ws, e.uri, e.path + "/");
                    else match(e);
                }
            } catch (IOException | SecurityException e) {
                skipped++;
            } finally {
                ancestors.remove(dir);
            }
        }

        void match(Entry e) {
            if (contents) scanFile(e);
            else if (e.path.toLowerCase(Locale.ROOT).contains(query)) add(e);
        }

        void add(Entry hit) {
            List<String> path = hit.uri.getPathSegments();
            int index = path.indexOf("document");
            String id = index >= 0 && index + 1 < path.size() ? path.get(index + 1) : hit.uri.toString();
            if (found.add(new Match(hit.uri.getAuthority(), id, hit.offset))) hits.add(hit);
        }

        void scanFile(Entry e) {
            try (InputStream input = resolver.openInputStream(e.uri)) {
                if (input == null) { skipped++; return; }
                long offset = 0;
                int n, matched = 0;
                while (hits.size() < LIMIT && (n = input.read(block)) >= 0) {
                    cancel.throwIfCanceled();
                    for (int i = 0; i < n; i++) {
                        while (matched > 0 && block[i] != term[matched]) matched = failure[matched - 1];
                        if (block[i] == term[matched] && ++matched == term.length) {
                            add(new Entry(e.name, e.path, e.uri, false, offset + i + 1 - term.length, term.length));
                            if (hits.size() == LIMIT) return;
                            matched = 0;
                        }
                    }
                    offset += n;
                }
            } catch (IOException | SecurityException ex) {
                skipped++;
            }
        }
    }
}
