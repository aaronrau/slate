package org.slate.editor;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract.Document;
import android.system.Os;
import android.system.OsConstants;

import java.io.*;

final class EditorFile implements AutoCloseable {
    final NativeDocument document = new NativeDocument();
    String name = "Untitled";
    Uri uri;
    boolean dirty;
    private File backing;
    private Metadata opened;

    record Metadata(String name, Long size, Long modified) {
        boolean changed(Metadata current) {
            return (size != null && current.size != null && !size.equals(current.size))
                || (modified != null && current.modified != null && !modified.equals(current.modified));
        }
    }

    private static Metadata metadata(ContentResolver resolver, Uri uri) {
        String name = uri.getLastPathSegment();
        Long size = null, modified = null;
        // A content URI supplied by Open With need not implement DocumentsProvider.
        try (Cursor c = resolver.query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int i = c.getColumnIndex(Document.COLUMN_DISPLAY_NAME);
                if (i >= 0 && !c.isNull(i)) name = c.getString(i);
                i = c.getColumnIndex(Document.COLUMN_SIZE);
                if (i >= 0 && !c.isNull(i)) size = c.getLong(i);
                i = c.getColumnIndex(Document.COLUMN_LAST_MODIFIED);
                if (i >= 0 && !c.isNull(i)) modified = c.getLong(i);
            }
        }
        return new Metadata(name == null ? "Document" : name, size, modified);
    }

    static EditorFile open(Context context, Uri uri) throws IOException {
        EditorFile file = new EditorFile();
        try {
            file.uri = uri;
            ContentResolver resolver = context.getContentResolver();
            file.opened = metadata(resolver, uri);
            file.name = file.opened.name;
            try (ParcelFileDescriptor fd = resolver.openFileDescriptor(uri, "r")) {
                if (fd == null) throw new IOException("Provider could not open this file");
                boolean regular;
                try {
                    regular = OsConstants.S_ISREG(Os.fstat(fd.getFileDescriptor()).st_mode);
                } catch (android.system.ErrnoException error) {
                    throw new IOException(error);
                }
                if (regular) {
                    file.document.open(fd.getFd());
                } else {
                    file.backing = File.createTempFile("slate-stream-", ".tmp", context.getCacheDir());
                    try (InputStream input = new FileInputStream(fd.getFileDescriptor());
                         OutputStream output = new FileOutputStream(file.backing)) {
                        transfer(input, output);
                    }
                    file.document.open(file.backing);
                }
            }
            return file;
        } catch (Exception e) {
            file.close();
            throw e;
        }
    }

    void save(Context context, Uri destination) throws IOException {
        ContentResolver resolver = context.getContentResolver();
        boolean same = destination.equals(uri);
        Metadata target = metadata(resolver, destination);
        if (same && opened != null && opened.changed(target))
            throw new IOException("This file changed in storage. Use Save as to keep your edits.");

        // Rebase the live model onto the complete staged bytes before a provider can
        // truncate the original descriptor. A failed transfer can then be retried.
        if (backing == null) {
            File stage = File.createTempFile("slate-save-", ".tmp", context.getCacheDir());
            try {
                document.save(stage);
                backing = stage;
            } finally {
                if (backing != stage) stage.delete();
            }
        } else {
            document.save(backing);
        }
        if (target.changed(metadata(resolver, destination)))
            throw new IOException("The destination changed during save. Try Save as.");

        try (FileInputStream input = new FileInputStream(backing);
             OutputStream output = resolver.openOutputStream(destination, "wt")) {
            if (output == null) throw new IOException("Provider declined the write");
            transfer(input, output);
        } catch (IOException e) {
            if (same) opened = metadata(resolver, destination);
            throw new IOException("Save did not finish. Your edited copy is still open. " + e.getMessage(), e);
        }
        opened = metadata(resolver, destination);
        name = opened.name;
        uri = destination;
        dirty = false;
        // Regular providers can become the backing again, releasing staging disk
        // space immediately. A pipe or unavailable read handle keeps the stage.
        try (ParcelFileDescriptor fd = resolver.openFileDescriptor(uri, "r")) {
            if (fd != null && fd.getStatSize() == document.size()
                    && OsConstants.S_ISREG(Os.fstat(fd.getFileDescriptor()).st_mode)) {
                document.open(fd.getFd());
                backing.delete();
                backing = null;
            }
        } catch (IOException | SecurityException | IllegalStateException | android.system.ErrnoException ignored) {
            // The complete staged model remains usable after the successful write.
        }
    }

    private static void transfer(InputStream input, OutputStream output) throws IOException {
        byte[] block = new byte[128 * 1024];
        int count;
        while ((count = input.read(block)) != -1) output.write(block, 0, count);
    }

    @Override public void close() {
        document.close();
        if (backing != null) backing.delete();
    }
}
