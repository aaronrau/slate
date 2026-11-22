package org.slate.editor;

import android.content.*;
import android.net.Uri;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.provider.MediaStore;
import android.test.ActivityInstrumentationTestCase2;
import android.view.*;
import android.widget.*;
import android.webkit.WebView;
import java.io.*;
import java.util.Arrays;
import java.util.concurrent.Callable;
import java.util.concurrent.atomic.AtomicReference;
import static java.nio.charset.StandardCharsets.UTF_8;

@SuppressWarnings("deprecation")
public final class EditorTest extends ActivityInstrumentationTestCase2<MainActivity> {
    public EditorTest() { super(MainActivity.class); }

    private <T> T onUi(Callable<T> work) throws Throwable {
        AtomicReference<T> value = new AtomicReference<>();
        runTestOnUiThread(() -> {
            try { value.set(work.call()); } catch (Exception ex) { throw new AssertionError(ex); }
        });
        return value.get();
    }

    private <T extends View> T find(View view, Class<T> type, String label) {
        if (!view.isShown()) return null;
        if (type.isInstance(view) && (label == null || label.contentEquals(
            view.getContentDescription() == null ? "" : view.getContentDescription()))) return type.cast(view);
        if (view instanceof ViewGroup group) for (int i = 0; i < group.getChildCount(); i++) {
            T candidate = find(group.getChildAt(i), type, label);
            if (candidate != null) return candidate;
        }
        return null;
    }

    private <T extends View> T view(Class<T> type, String label) {
        return find(getActivity().getWindow().getDecorView(), type, label);
    }
    private void await(Callable<Boolean> assertion) throws Throwable {
        long deadline = android.os.SystemClock.elapsedRealtime() + 20000;
        while (android.os.SystemClock.elapsedRealtime() < deadline) {
            if (onUi(assertion)) return;
            Thread.sleep(50);
        }
        fail("Editor did not reach expected state");
    }

    private Uri create(String ext, byte[] data) throws IOException {
        ContentResolver resolver = getInstrumentation().getTargetContext().getContentResolver();
        ContentValues metadata = new ContentValues();
        metadata.put(MediaStore.MediaColumns.DISPLAY_NAME, "Slate test " + System.nanoTime() + ext);
        metadata.put(MediaStore.MediaColumns.MIME_TYPE, ext.equals(".md") ? "text/markdown" : "text/plain");
        metadata.put(MediaStore.MediaColumns.RELATIVE_PATH, "Download/");
        Uri uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, metadata);
        assertNotNull(uri);
        try (OutputStream stream = resolver.openOutputStream(uri, "wt")) { stream.write(data); }
        return uri;
    }

    public void testUnboundedHistoryBranchAndPersistedPosition() {
        Context context = getInstrumentation().getTargetContext();
        String dbName = "test-navigation-" + System.nanoTime();
        Uri a = Uri.parse("content://test/a"), b = Uri.parse("content://test/b"), c = Uri.parse("content://test/c");
        try {
            try (Navigation history = new Navigation(context, dbName)) {
                android.database.sqlite.SQLiteDatabase db = history.getWritableDatabase();
                db.beginTransaction();
                try {
                    for (int i = 0; i < 4096; i++) {
                        Uri target = (i % 2 == 0) ? a : b;
                        history.visit(target, target.getLastPathSegment(), ((long)i << 32), i % 2 == 1);
                    }
                    db.setTransactionSuccessful();
                } finally { db.endTransaction(); }
                assertEquals(2, history.tabCount());
                assertEquals(4096L, history.relative(0).visit());
                history.activate(history.relative(-1).visit());
                history.activate(history.relative(-1).visit());
                history.visit(c, "C", 756L, true);
                assertEquals(3, history.tabCount());
                assertNull(history.relative(1));
                assertEquals(4095L, history.relative(0).visit());
                history.unavailable(history.relative(-1).tab());
                assertEquals(a, history.relative(-1).uri());
            }
            try (Navigation restored = new Navigation(context, dbName)) {
                assertEquals(c, restored.relative(0).uri());
                assertTrue(restored.relative(0).preview());
                assertEquals(756L, restored.relative(0).offset());
                assertEquals(b, restored.relative(-1).uri());
                assertEquals((4093L << 32), restored.relative(-1).offset());
            }
        } finally { context.deleteDatabase(dbName); }
    }

    public void testNativeSparseDescriptorAndUtf8Edits() throws Exception {
        Context context = getInstrumentation().getTargetContext();
        File input = File.createTempFile("slate-test", ".txt", context.getCacheDir());
        File output = File.createTempFile("slate-out", ".txt", context.getCacheDir());
        long far = 5L << 30;
        try (NativeDocument doc = new NativeDocument()) {
            try (RandomAccessFile data = new RandomAccessFile(input, "rw")) {
                data.seek(far);
                data.write("🌱\r\nfin".getBytes(UTF_8));
            }
            try (ParcelFileDescriptor fd = ParcelFileDescriptor.open(input, ParcelFileDescriptor.MODE_READ_ONLY)) {
                doc.open(fd.getFd());
            }
            assertEquals(far + 9, doc.size());
            assertEquals(far + 4, doc.boundary(far + 1));
            assertEquals(far + 6, doc.boundary(far + 5));
            doc.replace(far + 6, 3, "end".getBytes(UTF_8));
            assertEquals("🌱\r\nend", new String(doc.read(far, 10), UTF_8));
            doc.replace(0, far, new byte[0]);
            doc.save(output);
            assertEquals(9L, output.length());
            try (NativeDocument reopened = new NativeDocument()) {
                reopened.open(output);
                assertEquals("🌱\r\nend", new String(reopened.read(0, 9), UTF_8));
            }
        } finally { input.delete(); output.delete(); }
    }

    public void testEditSaveBrowseHistoryAndOfflineMarkdown() throws Throwable {
        if (Build.VERSION.SDK_INT < 29) return;
        byte[] text = new byte[44000];
        Arrays.fill(text, (byte)'a');
        Uri a = create(".txt", text), b = create(".md",
            ("# Diagram\n\n| A | B |\n|---|---|\n| 1 | 2 |\n\n"
                + "```mermaid\nflowchart LR\nA-->B\n```\n").getBytes(UTF_8));
        ContentResolver resolver = getInstrumentation().getTargetContext().getContentResolver();
        try {
            setActivityIntent(new Intent(Intent.ACTION_EDIT, a));
            MainActivity activity = getActivity();
            await(() -> view(Button.class, "More options").isEnabled() && view(EditText.class, null) != null && view(EditText.class, null).length() == 32768);
            onUi(() -> { activity.requestOpen(a, 43500, 0, null); return null; });
            await(() -> view(EditText.class, null) != null && view(EditText.class, null).length() == 44000 - 32768);
            onUi(() -> {
                EditText edit = view(EditText.class, null);
                edit.getText().insert(43500 - 32768, "🚲");
                view(Button.class, "Save file").performClick();
                return null;
            });
            await(() -> view(Button.class, "More options").isEnabled() && !view(Button.class, "Save file").isEnabled());
            try (InputStream stream = resolver.openInputStream(a)) {
                ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                byte[] block = new byte[8192]; int n;
                while ((n = stream.read(block)) >= 0) bytes.write(block, 0, n);
                assertEquals(44004, bytes.size());
                assertEquals("🚲", new String(bytes.toByteArray(), 43500, 4, UTF_8));
            }
            onUi(() -> { activity.requestOpen(b, -1, 0, null); return null; });
            await(() -> view(WebView.class, null) != null);
            AtomicReference<String> dom = new AtomicReference<>("");
            await(() -> {
                WebView web = view(WebView.class, null);
                web.evaluateJavascript("Boolean(document.querySelector('table td') && document.querySelector('.diagram svg'))", dom::set);
                return "true".equals(dom.get());
            });
            onUi(() -> view(Button.class, "Previous file").performClick());
            await(() -> view(EditText.class, null) != null && view(EditText.class, null).getText().toString().contains("🚲"));
            onUi(() -> view(Button.class, "Next file").performClick());
            await(() -> view(WebView.class, null) != null);
        } finally {
            resolver.delete(a, null, null);
            resolver.delete(b, null, null);
        }
    }
}
