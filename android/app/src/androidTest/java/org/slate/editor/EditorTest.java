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
    private void await(String state, Callable<Boolean> assertion) throws Throwable {
        android.util.Log.i("SlateEditorTest", "Waiting for " + state);
        long deadline = android.os.SystemClock.elapsedRealtime() + 60000;
        while (android.os.SystemClock.elapsedRealtime() < deadline) {
            if (onUi(assertion)) return;
            Thread.sleep(50);
        }
        StringBuilder details = new StringBuilder();
        onUi(() -> {
            View window = getActivity().getWindow().getDecorView();
            details.append(" windowFocus=").append(window.hasWindowFocus())
                .append(" windowVisibility=").append(window.getWindowVisibility());
            describe(window, details);
            return null;
        });
        android.view.accessibility.AccessibilityNodeInfo active =
            getInstrumentation().getUiAutomation().getRootInActiveWindow();
        if (active != null) {
            for (android.view.accessibility.AccessibilityNodeInfo node : active.findAccessibilityNodeInfosByViewId("android:id/message"))
                details.append(" dialog=").append(node.getText());
        }
        fail("Timed out waiting for " + state + "." + details);
    }

    private void describe(View view, StringBuilder details) {
        if (view instanceof EditText editor) details.append(" editorChars=").append(editor.length());
        else if (view instanceof TextView label && label.isShown()) {
            details.append(" [").append(label.getText()).append("]");
        } else if (view instanceof WebView web) {
            details.append(" WebView=").append(web.getUrl());
        }
        if (view instanceof ViewGroup group)
            for (int i = 0; i < group.getChildCount(); i++) describe(group.getChildAt(i), details);
    }

    private Uri create(String ext, byte[] data) throws IOException {
        ContentResolver resolver = getInstrumentation().getTargetContext().getContentResolver();
        ContentValues metadata = new ContentValues();
        metadata.put(MediaStore.MediaColumns.DISPLAY_NAME, "Slate test " + System.nanoTime() + ext);
        metadata.put(MediaStore.MediaColumns.MIME_TYPE, ext.equals(".md") ? "text/markdown" : "text/plain");
        metadata.put(MediaStore.MediaColumns.RELATIVE_PATH, "Download/");
        metadata.put(MediaStore.MediaColumns.IS_PENDING, 1);
        Uri uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, metadata);
        assertNotNull(uri);
        try (OutputStream stream = resolver.openOutputStream(uri, "wt")) { stream.write(data); }
        metadata.clear();
        metadata.put(MediaStore.MediaColumns.IS_PENDING, 0);
        assertEquals(1, resolver.update(uri, metadata, null, null));
        return uri;
    }

    public void testConcurrentProviderWriteIsPreserved() throws Exception {
        if (Build.VERSION.SDK_INT < 29) return;
        Context context = getInstrumentation().getTargetContext();
        ContentResolver resolver = context.getContentResolver();
        Uri uri = create(".txt", "original".getBytes(UTF_8));
        try (EditorFile editor = EditorFile.open(context, uri)) {
            editor.document.replace(0, 8, "edited".getBytes(UTF_8));
            try (OutputStream output = resolver.openOutputStream(uri, "wt")) {
                output.write("another writer".getBytes(UTF_8));
            }
            try {
                editor.save(context, uri);
                fail("Saving after an external write must report a conflict");
            } catch (IOException expected) {
                assertTrue(expected.getMessage().contains("changed in storage"));
            }
            try (java.io.DataInputStream input = new java.io.DataInputStream(resolver.openInputStream(uri))) {
                byte[] bytes = new byte[14];
                input.readFully(bytes);
                assertEquals("another writer", new String(bytes, UTF_8));
                assertEquals(-1, input.read());
            }
        } finally { resolver.delete(uri, null, null); }
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
                restored.remove(c);
                assertEquals(b, restored.relative(0).uri());
                assertNull(restored.relative(1));
                restored.remove(a);
                assertEquals(1, restored.tabCount());
                assertEquals(b, restored.tabAt(0).uri());
                assertEquals(0, restored.indexOf(restored.relative(0).tab()));
            }
            try (Navigation again = new Navigation(context, dbName)) {
                assertEquals(b, again.relative(0).uri());
                assertEquals((4093L << 32), again.relative(0).offset());
            }
        } finally { context.deleteDatabase(dbName); }
    }

    public void testMigratingNavigationAndRemovingAlternateGrants() {
        Context context = getInstrumentation().getTargetContext();
        String name = "navigation-v1-" + System.nanoTime();
        Uri loose = Uri.parse("content://provider/document/a%2Fb"),
            tree = Uri.parse("content://provider/tree/a/document/a%2Fb"),
            kept = Uri.parse("content://provider/document/c");
        try {
            try (android.database.sqlite.SQLiteDatabase old = context.openOrCreateDatabase(name, Context.MODE_PRIVATE, null)) {
                old.execSQL("CREATE TABLE tabs(id INTEGER PRIMARY KEY, uri TEXT NOT NULL UNIQUE, name TEXT NOT NULL, offset INTEGER NOT NULL, preview INTEGER NOT NULL)");
                old.execSQL("CREATE TABLE visits(id INTEGER PRIMARY KEY, tab INTEGER NOT NULL, offset INTEGER NOT NULL, preview INTEGER NOT NULL)");
                old.execSQL("CREATE TABLE position(visit INTEGER NOT NULL)");
                old.execSQL("INSERT INTO position VALUES(3)");
                int tab = 0;
                for (Uri uri : new Uri[]{loose, kept, tree}) {
                    tab++;
                    old.execSQL("INSERT INTO tabs VALUES(?,?,?,?,0)", new Object[]{tab, uri.toString(), "File " + tab, tab * 20});
                    old.execSQL("INSERT INTO visits VALUES(?,?,?,0)", new Object[]{tab, tab, tab * 20});
                }
                old.setVersion(1);
            }
            try (Navigation migrated = new Navigation(context, name)) {
                assertEquals(tree, migrated.relative(0).uri());
                assertEquals(3, migrated.tabCount());
                migrated.remove(tree);
                assertEquals(1, migrated.tabCount());
                assertEquals(kept, migrated.tabAt(0).uri());
                assertEquals(40L, migrated.relative(0).offset());
                assertNull(migrated.relative(1));
                assertNull(migrated.relative(-1));
            }
        } finally { context.deleteDatabase(name); }
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
            await("opening first source page", () -> view(Button.class, "More options").isEnabled() && view(EditText.class, null) != null && view(EditText.class, null).length() == 32768);
            onUi(() -> { activity.requestOpen(a, 43500, 0, null); return null; });
            await("loading final source page", () -> view(EditText.class, null) != null && view(EditText.class, null).length() == 44000 - 32768);
            onUi(() -> {
                EditText edit = view(EditText.class, null);
                edit.getText().insert(43500 - 32768, "🚲");
                view(Button.class, "Save file").performClick();
                return null;
            });
            await("provider save completion", () -> view(Button.class, "More options").isEnabled() && !view(Button.class, "Save file").isEnabled());
            try (InputStream stream = resolver.openInputStream(a)) {
                ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                byte[] block = new byte[8192]; int n;
                while ((n = stream.read(block)) >= 0) bytes.write(block, 0, n);
                assertEquals(44004, bytes.size());
                assertEquals("🚲", new String(bytes.toByteArray(), 43500, 4, UTF_8));
            }
            onUi(() -> { activity.requestOpen(b, -1, 0, null); return null; });
            await("showing Markdown preview", () -> view(WebView.class, null) != null);
            AtomicReference<String> dom = new AtomicReference<>("");
            await("rendering table cells and Mermaid SVG", () -> {
                WebView web = view(WebView.class, null);
                web.evaluateJavascript("[document.readyState, typeof window.slatePreview, !!document.querySelector('table td'), " +
                    "!!document.querySelector('.diagram svg'), document.querySelector('.diagram details')?.textContent?.slice(0,100)]",
                    value -> {
                        if (!value.equals(dom.getAndSet(value)))
                            android.util.Log.i("SlateEditorTest", "Preview renderer " + value);
                    });
                return dom.get().startsWith("[\"complete\",\"object\",true,true");
            });
            onUi(() -> view(Button.class, "Previous file").performClick());
            await("returning to the edited source page", () -> view(EditText.class, null) != null && view(EditText.class, null).getText().toString().contains("🚲"));
            onUi(() -> view(Button.class, "Next file").performClick());
            await("restoring the preview via Forward", () -> view(WebView.class, null) != null);
        } finally {
            resolver.delete(a, null, null);
            resolver.delete(b, null, null);
        }
    }
}
