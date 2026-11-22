package org.slate.editor;

import android.app.Instrumentation;
import android.app.UiAutomation;
import android.content.*;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import android.test.ActivityInstrumentationTestCase2;
import android.view.*;
import android.view.accessibility.AccessibilityNodeInfo;
import android.widget.*;

import java.io.*;
import java.util.*;
import java.util.concurrent.Callable;
import java.util.concurrent.atomic.AtomicReference;

@SuppressWarnings("deprecation")
public final class FilesPaneTest extends ActivityInstrumentationTestCase2<MainActivity> {
    public FilesPaneTest() { super(MainActivity.class); }

    private <T> T main(Callable<T> work) throws Throwable {
        AtomicReference<T> value = new AtomicReference<>();
        runTestOnUiThread(() -> {
            try { value.set(work.call()); } catch (Exception e) { throw new AssertionError(e); }
        });
        return value.get();
    }

    private void await(String label, Callable<Boolean> work) throws Exception {
        long deadline = SystemClock.elapsedRealtime() + 45000;
        while (SystemClock.elapsedRealtime() < deadline) {
            if (work.call()) return;
            Thread.sleep(100);
        }
        fail("Timed out waiting for " + label);
    }

    private View find(View view, String description) {
        if (!view.isShown()) return null;
        if (description.contentEquals(view.getContentDescription() == null ? "" : view.getContentDescription())) return view;
        if (view instanceof ViewGroup group) for (int i = 0; i < group.getChildCount(); i++) {
            View candidate = find(group.getChildAt(i), description);
            if (candidate != null) return candidate;
        }
        return null;
    }

    private View view(String description) { return find(getActivity().getWindow().getDecorView(), description); }

    private void waitForView(String description) throws Exception {
        await(description, () -> {
            AtomicReference<Boolean> present = new AtomicReference<>(false);
            getInstrumentation().runOnMainSync(() -> present.set(view(description) != null));
            return present.get();
        });
    }

    private void tapRow(String label) throws Throwable {
        waitForView(label);
        main(() -> {
            ListView list = (ListView)view("Workspace files");
            View child = view(label);
            int i = list.getPositionForView(child);
            assertTrue(i >= 0);
            return list.performItemClick(child, i, list.getItemIdAtPosition(i));
        });
    }

    private int hit(long offset) {
        ListView matches = (ListView)view("Search results");
        if (matches == null) return -1;
        for (int i = 0; i < matches.getCount(); i++) {
            Workspace.Entry e = (Workspace.Entry)matches.getItemAtPosition(i);
            if (e.path().endsWith("nested/entry.txt") && e.offset() == offset) return i;
        }
        return -1;
    }

    private void waitForHit(long offset) throws Exception {
        await("recursive search result at " + offset, () -> {
            AtomicReference<Boolean> found = new AtomicReference<>(false);
            getInstrumentation().runOnMainSync(() -> found.set(hit(offset) >= 0));
            return found.get();
        });
    }

    private void shell(String command) throws IOException {
        ParcelFileDescriptor fd = getInstrumentation().getUiAutomation().executeShellCommand(command);
        try (InputStream in = new ParcelFileDescriptor.AutoCloseInputStream(fd)) {
            byte[] block = new byte[128];
            while (in.read(block) >= 0) { /* Wait for completion. */ }
        }
    }

    // Click the native DocumentsUI control. IDs survive translations and the
    // plain text fallback also covers provider UI variants without stable IDs.
    private void pickerButton(String idSuffix, String text) throws Exception {
        UiAutomation automation = getInstrumentation().getUiAutomation();
        await(text, () -> {
            AccessibilityNodeInfo root = automation.getRootInActiveWindow();
            if (root == null) return false;
            ArrayDeque<AccessibilityNodeInfo> nodes = new ArrayDeque<>();
            nodes.add(root);
            while (!nodes.isEmpty()) {
                AccessibilityNodeInfo node = nodes.removeFirst();
                String id = node.getViewIdResourceName();
                CharSequence title = node.getText();
                if (node.isEnabled() && ((idSuffix != null && id != null && id.endsWith(idSuffix))
                    || (title != null && text.equalsIgnoreCase(title.toString())))) {
                    while (node != null) {
                        if (node.isClickable()) return node.performAction(AccessibilityNodeInfo.ACTION_CLICK);
                        node = node.getParent();
                    }
                    return false;
                }
                for (int i = 0; i < node.getChildCount(); i++) {
                    AccessibilityNodeInfo child = node.getChild(i);
                    if (child != null) nodes.add(child);
                }
            }
            return false;
        });
    }

    private void enterName(String value) throws Exception {
        UiAutomation automation = getInstrumentation().getUiAutomation();
        await("file name entry", () -> {
            AccessibilityNodeInfo root = automation.getRootInActiveWindow();
            if (root == null) return false;
            ArrayDeque<AccessibilityNodeInfo> nodes = new ArrayDeque<>();
            nodes.add(root);
            while (!nodes.isEmpty()) {
                AccessibilityNodeInfo n = nodes.removeFirst();
                if ("New file name".contentEquals(n.getContentDescription() == null ? "" : n.getContentDescription())) {
                    android.os.Bundle args = new android.os.Bundle();
                    args.putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, value);
                    return n.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, args);
                }
                for (int i = 0; i < n.getChildCount(); i++) if (n.getChild(i) != null) nodes.add(n.getChild(i));
            }
            return false;
        });
    }

    private EditText source(View v) {
        if (v instanceof EditText e && e.isShown() && !"Search files".equals(e.getContentDescription())) return e;
        if (v instanceof ViewGroup g) for (int i = 0; i < g.getChildCount(); i++) {
            EditText e = source(g.getChildAt(i));
            if (e != null) return e;
        }
        return null;
    }

    private void manageNestedFile(MainActivity activity, Uri grant, String docId) throws Throwable {
        Workspace ws = new Workspace(activity.getContentResolver(), grant);
        Uri nested = DocumentsContract.buildDocumentUriUsingTree(grant, docId + "/nested");
        Uri note = DocumentsContract.buildDocumentUriUsingTree(grant, docId + "/nested/notes.md");
        waitForView("Actions for nested");
        main(() -> view("Actions for nested").performClick());
        pickerButton(null, "New file here…");
        enterName("notes.md");
        pickerButton(null, "Create");
        waitForView("notes.md, current file");
        assertEquals("notes.md", Workspace.info(activity.getContentResolver(), note).name());
        main(() -> {
            source(activity.getWindow().getDecorView()).setText("saved text");
            return view("Save file").performClick();
        });
        await("saved provider bytes", () -> {
            try (InputStream content = activity.getContentResolver().openInputStream(note)) {
                byte[] b = new byte[10];
                return content != null && content.read(b) == b.length && new String(b, java.nio.charset.StandardCharsets.UTF_8).equals("saved text");
            }
        });
        try { ws.create(nested, "notes.md"); fail("Existing file was accepted"); }
        catch (IOException expected) { assertTrue(expected.getMessage().contains("already exists")); }
        for (String other : new String[]{"Makefile", ".env", "custom.abcxyz"}) {
            Workspace.Created created = ws.create(nested, other);
            assertEquals(other, Workspace.info(activity.getContentResolver(), created.uri()).name());
            Workspace.delete(activity, created.uri());
        }
        try { Workspace.delete(activity, nested); fail("Directory deletion was accepted"); }
        catch (IOException expected) { assertTrue(expected.getMessage().contains("Choose a file")); }
        main(() -> {
            source(activity.getWindow().getDecorView()).append(" unsaved");
            return view("Actions for notes.md").performClick();
        });
        pickerButton(null, "Delete file…");
        pickerButton(null, "Cancel");
        assertEquals("saved text unsaved", main(() -> source(activity.getWindow().getDecorView()).getText().toString()));
        assertEquals("notes.md", Workspace.info(activity.getContentResolver(), note).name());
        main(() -> view("Actions for notes.md").performClick());
        pickerButton(null, "Delete file…");
        pickerButton(null, "Delete and discard edits");
        waitForView("entry.txt, current file");
        assertNull(main(() -> view("Actions for notes.md")));
        assertFalse(ws.list(nested, 0, null).entries().stream().anyMatch(e -> e.name().equals("notes.md")));
    }

    public void testGrantedFolderOpensAsAdaptiveInlineTree() throws Throwable {
        String name = "SlateUITest" + System.nanoTime();
        String fixture = "/sdcard/Documents/" + name;
        String docId = "primary:Documents/" + name;
        String authority = "com.android.externalstorage.documents";
        Uri initial = DocumentsContract.buildDocumentUri(authority, docId);
        Uri grant = DocumentsContract.buildTreeDocumentUri(authority, docId);
        shell("mkdir -p " + fixture + "/nested");
        shell("cp /system/etc/hosts " + fixture + "/nested/entry.txt");
        Instrumentation instrumentation = getInstrumentation();
        Instrumentation.ActivityMonitor monitor = new Instrumentation.ActivityMonitor() {
            @Override public Instrumentation.ActivityResult onStartActivity(Intent intent) {
                if (Intent.ACTION_OPEN_DOCUMENT_TREE.equals(intent.getAction()))
                    intent.putExtra(DocumentsContract.EXTRA_INITIAL_URI, initial);
                return null;
            }
        };
        instrumentation.addMonitor(monitor);
        try {
            MainActivity activity = getActivity();
            View root = main(() -> ((ViewGroup)activity.findViewById(android.R.id.content)).getChildAt(0));
            main(() -> {
                ViewGroup.LayoutParams params = root.getLayoutParams();
                params.width = activity.dp(390);
                root.setLayoutParams(params);
                return null;
            });
            waitForView("Files");
            main(() -> view("Files").performClick());
            waitForView("Open folder");
            assertNotNull(main(() -> view("Open file")));
            main(() -> view("Open folder").performClick());
            pickerButton(":id/action_menu_select", "Use this folder");
            pickerButton("android:id/button1", "Allow");
            waitForView(name + ", expanded folder");
            assertTrue(activity.getContentResolver().getPersistedUriPermissions().stream()
                .anyMatch(p -> p.isReadPermission() && p.getUri().equals(grant)));
            tapRow("nested, collapsed folder");
            tapRow("entry.txt");
            waitForView("Files");
            await("source editor after file selection", () -> {
                AtomicReference<Boolean> closed = new AtomicReference<>(false);
                instrumentation.runOnMainSync(() -> closed.set(view("Files pane") == null));
                return closed.get();
            });
            main(() -> view("Files").performClick());
            waitForView("entry.txt, current file");
            main(() -> { ((EditText)view("Search files")).setText("entry.txt"); return null; });
            waitForHit(-1);
            main(() -> {
                ((RadioButton)view("Search file contents")).setChecked(true);
                ((EditText)view("Search files")).setText("localhost");
                return null;
            });
            byte[] hosts = java.nio.file.Files.readAllBytes(java.nio.file.Paths.get("/system/etc/hosts"));
            long match = new String(hosts, java.nio.charset.StandardCharsets.UTF_8).indexOf("localhost");
            assertTrue(match >= 0);
            waitForHit(match);
            main(() -> {
                ListView results = (ListView)view("Search results");
                int i = hit(match);
                return results.performItemClick(null, i, results.getItemIdAtPosition(i));
            });
            waitForView("Files");
            main(() -> view("Files").performClick());
            waitForView("Clear search");
            main(() -> view("Clear search").performClick());
            waitForView("entry.txt, current file");
            tapRow("nested, expanded folder");
            assertNull(main(() -> view("entry.txt, current file")));
            // Resize just this Activity's content. The device and its display
            // settings remain untouched on both physical and virtual devices.
            main(() -> {
                ViewGroup.LayoutParams params = root.getLayoutParams();
                params.width = activity.dp(900);
                root.setLayoutParams(params);
                return null;
            });
            await("persistent pane in wide window", () -> {
                AtomicReference<Boolean> wide = new AtomicReference<>(false);
                instrumentation.runOnMainSync(() -> wide.set(view("Files") == null && view("Files pane") != null));
                return wide.get();
            });
            tapRow("nested, collapsed folder");
            tapRow("entry.txt, current file");
            assertNotNull(main(() -> view("Files pane")));
            manageNestedFile(activity, grant, docId);
        } finally {
            instrumentation.removeMonitor(monitor);
            for (UriPermission permission : instrumentation.getTargetContext().getContentResolver().getPersistedUriPermissions()) {
                if (grant.equals(permission.getUri())) instrumentation.getTargetContext().getContentResolver()
                    .releasePersistableUriPermission(grant, Intent.FLAG_GRANT_READ_URI_PERMISSION |
                        (permission.isWritePermission() ? Intent.FLAG_GRANT_WRITE_URI_PERMISSION : 0));
            }
            shell("rm -rf " + fixture);
        }
    }
}
