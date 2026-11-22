package org.slate.editor;

import android.content.ContentResolver;
import android.content.Context;
import android.webkit.MimeTypeMap;
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

    static String documentKey(Uri uri) {
        List<String> parts = uri.getPathSegments();
        int index = parts.indexOf("document");
        return index >= 0 && index + 1 < parts.size()
            ? DocumentsContract.buildDocumentUri(uri.getAuthority(), parts.get(index + 1)).toString() : uri.toString();
    }

    static String invalidName(String name) {
        if (name.trim().isEmpty() || name.equals(".") || name.equals("..")) return "Enter a file name.";
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if (c == '/' || c == '\\' || Character.isISOControl(c))
                return "Use a file name without paths or control characters.";
        }
        return null;
    }

    record Info(String name, String mime, int flags, EditorFile.Metadata metadata) {}

    static Info info(ContentResolver resolver, Uri doc) throws IOException {
        String[] fields = {Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE, Document.COLUMN_FLAGS,
            Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED};
        try (Cursor c = resolver.query(doc, fields, null, null, null)) {
            if (c == null || !c.moveToFirst()) throw new FileNotFoundException("This item is no longer available.");
            String name = c.getString(c.getColumnIndexOrThrow(Document.COLUMN_DISPLAY_NAME));
            int size = c.getColumnIndex(Document.COLUMN_SIZE), modified = c.getColumnIndex(Document.COLUMN_LAST_MODIFIED);
            EditorFile.Metadata metadata = new EditorFile.Metadata(name, size < 0 || c.isNull(size) ? null : c.getLong(size),
                modified < 0 || c.isNull(modified) ? null : c.getLong(modified));
            return new Info(name, c.getString(c.getColumnIndexOrThrow(Document.COLUMN_MIME_TYPE)),
                c.getInt(c.getColumnIndexOrThrow(Document.COLUMN_FLAGS)), metadata);
        }
    }

    record Created(Uri uri, int pageStart) {}

    Created create(Uri parent, String name) throws IOException {
        String problem = invalidName(name);
        if (problem != null) throw new IOException(problem);
        Info folder = info(resolver, parent);
        if (!Document.MIME_TYPE_DIR.equals(folder.mime) || (folder.flags & Document.FLAG_DIR_SUPPORTS_CREATE) == 0)
            throw new IOException("This folder does not allow new files.");
        try (Cursor c = children(parent, null)) {
            while (c.moveToNext()) if (name.equals(c.getString(1)))
                throw new IOException("A file or folder named “" + name + "” already exists.");
        }
        int dot = name.lastIndexOf('.');
        String mime = dot > 0 && dot < name.length() - 1
            ? MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substring(dot + 1).toLowerCase(Locale.ROOT)) : null;
        Uri created;
        try {
            created = DocumentsContract.createDocument(resolver, parent, mime == null ? "application/octet-stream" : mime, name);
        } catch (SecurityException denied) { throw new IOException("Slate does not have write access to this folder.", denied); }
        if (created == null) throw new IOException("The provider could not create this file.");
        // Reveal the new row even when the provider places it on a later
        // directory page. Only the cursor window is held while locating it.
        String id = DocumentsContract.getDocumentId(created);
        try (Cursor c = children(parent, null)) {
            while (c.moveToNext()) if (id.equals(c.getString(0))) return new Created(created, c.getPosition() / PAGE * PAGE);
        } catch (IOException | RuntimeException ignored) { /* The file was created. */ }
        return new Created(created, -1);
    }

    record Deletion(Uri uri, String name, String mime, EditorFile.Revision revision) {}

    static Deletion prepareDelete(Context context, Uri target, String displayName) throws IOException {
        if (!DocumentsContract.isDocumentUri(context, target))
            throw new IOException("This provider does not support deleting this file in Slate.");
        ContentResolver resolver = context.getContentResolver();
        Info doc;
        try {
            doc = info(resolver, target);
        } catch (SecurityException denied) {
            // Some providers grant deletion and expose the MIME type without
            // granting access to the metadata cursor or file bytes.
            doc = new Info(displayName, resolver.getType(target), Document.FLAG_SUPPORTS_DELETE,
                new EditorFile.Metadata(displayName, null, null));
        }
        if (doc.mime == null || Document.MIME_TYPE_DIR.equals(doc.mime)) throw new IOException("Choose a file to delete.");
        if ((doc.flags & Document.FLAG_SUPPORTS_DELETE) == 0) throw new IOException("This file is read-only.");
        boolean inspectFile = (doc.flags & Document.FLAG_VIRTUAL_DOCUMENT) == 0;
        return new Deletion(target, doc.name == null ? displayName : doc.name, doc.mime,
            EditorFile.deletionRevision(resolver, target, doc.metadata, inspectFile));
    }

    static void delete(Context context, Deletion confirmed) throws IOException {
        Deletion current = prepareDelete(context, confirmed.uri, confirmed.name);
        if (!confirmed.name.equals(current.name) || !confirmed.mime.equals(current.mime)
            || confirmed.revision.cannotConfirm(current.revision))
            throw new IOException("The file changed while deletion was being confirmed. Check it before trying again.");
        try {
            if (!DocumentsContract.deleteDocument(context.getContentResolver(), confirmed.uri))
                throw new IOException("The provider did not delete this file.");
        } catch (SecurityException denied) { throw new IOException("Slate does not have permission to delete this file.", denied); }
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
