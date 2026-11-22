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
        AccessibilityNodeInfo root = getInstrumentation().getUiAutomation().getRootInActiveWindow();
        List<String> labels = new ArrayList<>();
        if (root != null) {
            ArrayDeque<AccessibilityNodeInfo> nodes = new ArrayDeque<>();
            nodes.add(root);
            while (!nodes.isEmpty() && labels.size() < 40) {
                AccessibilityNodeInfo n = nodes.removeFirst();
                if (n.getText() != null) labels.add(n.getText().toString());
                else if (n.getContentDescription() != null) labels.add(n.getContentDescription().toString());
                for (int i = 0; i < n.getChildCount(); ++i) {
                    AccessibilityNodeInfo child = n.getChild(i);
                    if (child != null) nodes.addLast(child);
                }
            }
        }
        fail("Timed out waiting for " + label + ". Window: "
            + (root == null ? "none" : root.getPackageName()) + " " + labels);
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

    private String shell(String command) throws IOException {
        ParcelFileDescriptor fd = getInstrumentation().getUiAutomation().executeShellCommand(command);
        try (InputStream in = new ParcelFileDescriptor.AutoCloseInputStream(fd)) {
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            byte[] block = new byte[256];
            for (int n; (n = in.read(block)) != -1;) output.write(block, 0, n);
            return output.toString("UTF-8").trim();
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
                if (node.isEnabled() && ((id != null && id.endsWith(idSuffix))
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

    public void testGrantedFolderOpensAsAdaptiveInlineTree() throws Throwable {
        String name = "SlateUITest" + System.nanoTime();
        String fixture = "/sdcard/Documents/" + name;
        String docId = "primary:Documents/" + name;
        String authority = "com.android.externalstorage.documents";
        Uri initial = DocumentsContract.buildDocumentUri(authority, docId);
        Uri grant = DocumentsContract.buildTreeDocumentUri(authority, docId);
        await("shared storage and the picker fixture", () -> {
            if (!android.os.Environment.MEDIA_MOUNTED.equals(android.os.Environment.getExternalStorageState())) return false;
            shell("mkdir -p " + fixture + "/nested");
            shell("cp /system/etc/hosts " + fixture + "/nested/entry.txt");
            return shell("cat " + fixture + "/nested/entry.txt").contains("localhost");
        });
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
            // The previous test can leave history pointing to a removed fixture.
            // Let Slate finish restoring that history before interacting with the drawer.
            await("editor ready for navigation", () -> {
                AtomicReference<Boolean> ready = new AtomicReference<>(false);
                instrumentation.runOnMainSync(() -> ready.set(activity.hasWindowFocus()
                    && view("Files") != null && view("Files").isEnabled()));
                return ready.get();
            });
            main(() -> view("Files").performClick());
            waitForView("Open folder");
            assertNotNull(main(() -> view("Open file")));
            main(() -> view("Open folder").performClick());
            // On a cold device DocumentsUI can briefly display its previous
            // location before applying EXTRA_INITIAL_URI. Confirm only our fixture.
            await("picker showing the nested fixture", () -> {
                AccessibilityNodeInfo window = instrumentation.getUiAutomation().getRootInActiveWindow();
                return window != null && !window.findAccessibilityNodeInfosByText("nested").isEmpty();
            });
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
