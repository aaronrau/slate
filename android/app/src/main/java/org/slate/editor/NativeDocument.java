package org.slate.editor;

import static java.nio.charset.StandardCharsets.UTF_8;
import java.io.File;

final class NativeDocument implements AutoCloseable {
    static { System.loadLibrary("slate_android"); }

    private static native long create();
    private static native void destroy(long document);
    private static native void openFd(long document, int fd);
    private static native void openFile(long document, byte[] path);
    private static native void saveFile(long document, byte[] path);
    private static native long length(long document);
    private static native long align(long document, long offset);
    private static native byte[] readBytes(long document, long offset, int length);
    private static native void edit(long document, long offset, long erased, byte[] inserted);

    private long handle = create();

    void open(int fd) { openFd(handle, fd); }
    void open(File path) { openFile(handle, path.getAbsolutePath().getBytes(UTF_8)); }
    void save(File path) { saveFile(handle, path.getAbsolutePath().getBytes(UTF_8)); }
    long size() { return length(handle); }
    long boundary(long offset) { return align(handle, offset); }
    byte[] read(long offset, int count) { return readBytes(handle, offset, count); }
    void replace(long offset, long erased, byte[] inserted) { edit(handle, offset, erased, inserted); }
    @Override public void close() {
        if (handle != 0) destroy(handle);
        handle = 0;
    }
}
