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
    final DirectoryIndex index;

    Workspace(Context context, Uri tree) {
        this(context.getContentResolver(), tree, DirectoryIndex.get(context));
    }

    Workspace(ContentResolver resolver, Uri tree, DirectoryIndex index) {
        this.resolver = resolver;
        this.index = index;
        this.tree = tree;
        root = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
    }

    Cursor children(Uri folder, CancellationSignal cancel) throws IOException {
        String id = DocumentsContract.getDocumentId(folder);
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
        Cursor cursor = resolver.query(children, COLUMNS, null, null, null, cancel);
        if (cursor == null) throw new IOException("Folder could not be read");
        return cursor;
    }

    Entry entry(DirectoryIndex.Row row, String prefix) {
        return new Entry(row.name(), prefix + row.name(), DocumentsContract.buildDocumentUriUsingTree(tree, row.id()),
            row.directory(), -1, 0);
    }

    Listing list(Uri folder, int skip, CancellationSignal cancel) throws IOException {
        if (cancel == null) cancel = new CancellationSignal();
        index.checkAccess(tree);
        DirectoryIndex.Window w = index.window(this, folder, skip + PAGE, true, cancel);
        ArrayList<Entry> entries = new ArrayList<>();
        for (DirectoryIndex.Row row : index.rows(w.id(), folder, skip, skip + PAGE, PAGE, cancel)) entries.add(entry(row, ""));
        entries.sort(Comparator.comparing(Entry::directory).reversed().thenComparing(Entry::name, String.CASE_INSENSITIVE_ORDER));
        return new Listing(entries, !w.complete() || w.count() > skip + PAGE);
    }

    // ExternalStorageProvider documents use volume:path IDs. Other providers own
    // their IDs and may supply an opaque value unrelated to their hierarchy.
    static final String EXTERNAL = "com.android.externalstorage.documents";
    static List<String> externalPath(Uri tree, Uri file) {
        if (!EXTERNAL.equals(tree.getAuthority()) || !EXTERNAL.equals(file.getAuthority())) return Collections.emptyList();
        String root = DocumentsContract.getTreeDocumentId(tree);
        String id;
        try { id = DocumentsContract.getDocumentId(file); } catch (IllegalArgumentException e) { return Collections.emptyList(); }
        String prefix = root.endsWith(":") ? root : root + "/";
        if (!id.startsWith(prefix)) return Collections.emptyList();
        ArrayList<String> path = new ArrayList<>();
        path.add(root);
        int slash = prefix.length();
        while ((slash = id.indexOf('/', slash)) >= 0) path.add(id.substring(0, slash++));
        path.add(id);
        return path;
    }

    static Uri parentHint(ContentResolver resolver, Uri file) {
        if (EXTERNAL.equals(file.getAuthority())) {
            try {
                String id = DocumentsContract.getDocumentId(file);
                int colon = id.indexOf(':'), slash = id.lastIndexOf('/');
                if (colon >= 0) return DocumentsContract.buildDocumentUri(EXTERNAL,
                    id.substring(0, slash > colon ? slash : colon + 1));
            } catch (IllegalArgumentException ignored) {}
        }
        try {
            DocumentsContract.Path path = DocumentsContract.findDocumentPath(resolver, file);
            if (path != null && path.getPath().size() >= 2) return DocumentsContract.buildDocumentUri(file.getAuthority(),
                path.getPath().get(path.getPath().size() - 2));
        } catch (Exception ignored) { /* A single-file grant may not expose any ancestor. */ }
        return file;
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

    record Created(Uri uri) {}

    Created create(Uri parent, String name) throws IOException {
        String problem = invalidName(name);
        if (problem != null) throw new IOException(problem);
        Info folder = info(resolver, parent);
        if (!Document.MIME_TYPE_DIR.equals(folder.mime) || (folder.flags & Document.FLAG_DIR_SUPPORTS_CREATE) == 0)
            throw new IOException("This folder does not allow new files.");
        index.checkAccess(tree);
        // A live check catches names created outside Slate since the last Refresh.
        // ExternalStorageProvider can resolve one candidate from its volume:path ID.
        boolean exists = false;
        boolean listSiblings = !EXTERNAL.equals(tree.getAuthority());
        if (EXTERNAL.equals(tree.getAuthority())) {
            Uri candidate = DocumentsContract.buildDocumentUriUsingTree(tree,
                DirectoryIndex.childPrefix(DocumentsContract.getDocumentId(parent)) + name);
            try { info(resolver,candidate); exists = true; }
            catch (FileNotFoundException missing) { /* This name is available. */ }
            catch (IllegalArgumentException unsupported) {
                // Some provider versions verify isChildDocument before checking
                // existence and return IllegalArgumentException for a missing child.
                listSiblings = true;
            }
        }
        if (listSiblings) {
            try (Cursor c = children(parent, null)) {
                while (c.moveToNext()) if (name.equals(c.getString(1))) { exists = true; break; }
            }
        }
        if (exists) throw new IOException("A file or folder named “" + name + "” already exists.");
        int dot = name.lastIndexOf('.');
        String mime = dot > 0 && dot < name.length() - 1
            ? MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substring(dot + 1).toLowerCase(Locale.ROOT)) : null;
        Uri created;
        try {
            created = DocumentsContract.createDocument(resolver, parent, mime == null ? "application/octet-stream" : mime, name);
        } catch (SecurityException denied) { throw new IOException("Slate does not have write access to this folder.", denied); }
        if (created == null) throw new IOException("The provider could not create this file.");
        try { index.created(tree,parent,created,info(resolver,created)); }
        catch (IOException | RuntimeException problemIndexing) {
            index.invalidateParent(tree,parent);
            android.util.Log.w("Slate", "Could not index the new file",problemIndexing);
        }
        return new Created(created);
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

    // Audio downloads can carry names such as "take.wav (2)" and a generic
    // MIME type. Prefer the provider's media type, then check the actual suffix.
    private static final Set<String> BINARY = new HashSet<>(Arrays.asList("wav","wave","mp3","mp4","m4a","m4b","flac","aac","aif","aiff","ogg",
        "opus","mid","midi","wma","amr","caf","au","webm","mov","mkv","avi","wmv","mpeg","m4v","jpg","jpeg","png",
        "gif","webp","heic","ico","bmp","tiff","raw","pdf","zip","gz","tgz","bz2","xz","7z","rar","tar","apk","jar",
        "exe","dll","so","class","dex","db","sqlite","sqlite3","woff","woff2","ttf","otf","doc","docx","xls","xlsx","ppt","pptx"));
    static boolean textCandidate(String name, String mime) {
        String lower = name.toLowerCase(Locale.ROOT);
        if (mime != null && (mime.startsWith("audio/") || mime.startsWith("video/")
            || mime.startsWith("image/") && !mime.equals("image/svg+xml") || mime.startsWith("font/")
            || mime.equals("application/pdf") || mime.equals("application/zip"))) return false;
        int end = lower.length();
        while (end > 3 && lower.charAt(end - 1) == ')') {
            int i = end - 2;
            while (i >= 0 && Character.isDigit(lower.charAt(i))) i--;
            if (i == end - 2 || i < 1 || lower.charAt(i) != '(' || lower.charAt(i-1) != ' ') break;
            end = i-1;
        }
        int dot = lower.lastIndexOf('.', end-1);
        return dot < 0 || !BINARY.contains(lower.substring(dot+1, end));
    }

    static Results find(ContentResolver resolver, List<Scope> folders, List<Entry> files, String term,
                        boolean contents, CancellationSignal cancel, java.util.function.Consumer<List<Entry>> progress) {
        Scan scan = new Scan(resolver, term, contents, cancel, progress);
        for (Scope scope : folders) {
            if (scan.hits.size() >= LIMIT) break;
            try {
                scope.workspace.index.checkAccess(scope.workspace.tree);
                scan.walk(scope.workspace, scope.workspace.root, folders.size() > 1 ? scope.name + "/" : "");
            } catch (SecurityException e) { scan.skipped++; }
        }
        for (Entry file : files) {
            if (scan.hits.size() >= LIMIT) break;
            cancel.throwIfCanceled();
            try {
                if (!contents || textCandidate(file.name, resolver.getType(file.uri))) scan.match(file);
            } catch (SecurityException denied) { scan.skipped++; }
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
        final java.util.function.Consumer<List<Entry>> progress;
        long lastUpdate;

        Scan(ContentResolver resolver, String query, boolean contents, CancellationSignal cancel, java.util.function.Consumer<List<Entry>> progress) {
            this.progress = progress;
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
            String folder = prefix.toLowerCase(Locale.ROOT);
            String needle = query.indexOf('/') >= 0 || folder.contains(query) ? "" : query;
            try {
                ws.index.visit(ws, dir, contents ? 2 : 3, needle, cancel, row -> {
                    match(ws.entry(row,prefix));
                    return hits.size() < LIMIT;
                });
                if (hits.size() < LIMIT) ws.index.visit(ws, dir, 1, "", cancel, row -> {
                    Entry e = ws.entry(row, prefix);
                    walk(ws, e.uri, e.path + "/");
                    return hits.size() < LIMIT;
                });
            } catch (IOException | SecurityException e) {
                skipped++;
            } finally {
                if (cancel.isCanceled()) ws.index.cancelSearch();
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
            if (found.add(new Match(hit.uri.getAuthority(), id, hit.offset))) {
                hits.add(hit);
                long now = android.os.SystemClock.uptimeMillis();
                if (progress != null && (hits.size() == 1 || now - lastUpdate >= 150)) {
                    progress.accept(new ArrayList<>(hits));
                    lastUpdate = now;
                }
            }
        }

        void scanFile(Entry e) {
            try (android.os.ParcelFileDescriptor fd = resolver.openFileDescriptor(e.uri, "r", cancel)) {
                if (fd == null) { skipped++; return; }
                try (InputStream input = new android.os.ParcelFileDescriptor.AutoCloseInputStream(fd)) {
                cancel.setOnCancelListener(() -> { try { input.close(); } catch (IOException ignored) {} });
                long offset = 0;
                int n, matched = 0;
                while (hits.size() < LIMIT && (n = input.read(block)) >= 0) {
                    cancel.throwIfCanceled();
                    if (offset == 0) for (int i = 0; i < n; i++) if (block[i] == 0) return;
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
                } finally { cancel.setOnCancelListener(null); }
            } catch (IOException | SecurityException ex) {
                skipped++;
            }
        }
    }
}
