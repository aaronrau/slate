package org.slate.editor;

import android.app.*;
import android.content.*;
import android.net.Uri;
import android.os.CancellationSignal;
import android.os.OperationCanceledException;
import android.text.InputType;
import android.widget.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.Consumer;

final class WorkspaceBrowser implements AutoCloseable {
    private final MainActivity ui;
    private final ExecutorService workers = Executors.newCachedThreadPool();
    private Workspace workspace;
    private CancellationSignal current;
    private AlertDialog dialog;

    WorkspaceBrowser(MainActivity ui) {
        this.ui = ui;
        String tree = ui.getPreferences(Context.MODE_PRIVATE).getString("tree", null);
        if (tree != null) workspace = new Workspace(ui.getContentResolver(), Uri.parse(tree));
    }

    boolean hasFolder() { return workspace != null; }

    void choose(Uri tree) {
        workspace = new Workspace(ui.getContentResolver(), tree);
        ui.getPreferences(Context.MODE_PRIVATE).edit().putString("tree", tree.toString()).apply();
        browse();
    }

    private interface Work<T> { T run(CancellationSignal stop) throws Exception; }
    private <T> void load(String title, Work<T> work, Consumer<T> done) {
        if (current != null) current.cancel();
        CancellationSignal stop = new CancellationSignal();
        current = stop;
        AlertDialog pending = ui.dialog().setTitle(title).setView(new ProgressBar(ui))
            .setNegativeButton("Cancel", (d, w) -> stop.cancel()).create();
        pending.setOnCancelListener(d -> stop.cancel());
        dialog = pending;
        pending.show();
        workers.execute(() -> {
            try {
                T result = work.run(stop);
                ui.runOnUiThread(() -> {
                    if (stop.isCanceled() || ui.isDestroyed()) return;
                    pending.dismiss();
                    done.accept(result);
                });
            } catch (OperationCanceledException ignored) {
            } catch (Exception ex) {
                ui.runOnUiThread(() -> {
                    if (stop.isCanceled() || ui.isDestroyed()) return;
                    pending.dismiss();
                    ui.error(ex);
                });
            }
        });
    }

    void browse() {
        if (workspace == null) { ui.pickFolder(); return; }
        showFolder(workspace.root, "Folder", new ArrayDeque<>(), 0);
    }

    private record Parent(Uri uri, String name, int skip) {}
    private void showFolder(Uri folder, String name, Deque<Parent> parents, int skip) {
        load("Opening folder", stop -> workspace.list(folder, skip, stop), listing -> {
            List<String> labels = new ArrayList<>();
            for (Workspace.Entry e : listing.entries()) labels.add(e.directory() ? e.name() + "/" : e.name());
            if (listing.more()) labels.add("Next files ›");
            if (skip > 0) labels.add("‹ Previous files");
            if (!parents.isEmpty()) labels.add("↑ Parent folder");
            AlertDialog.Builder b = ui.dialog().setTitle(name).setNegativeButton("Close", null)
                .setNeutralButton("Find", (d, w) -> find());
            if (labels.isEmpty()) b.setMessage("This folder is empty.");
            else b.setItems(labels.toArray(new String[0]), (d, index) -> {
                if (index < listing.entries().size()) {
                    Workspace.Entry e = listing.entries().get(index);
                    if (e.directory()) {
                        parents.push(new Parent(folder, name, skip));
                        showFolder(e.uri(), e.name(), parents, 0);
                    } else ui.requestOpen(e.uri(), -1, 0, null);
                    return;
                }
                String label = labels.get(index);
                if (label.startsWith("Next")) showFolder(folder, name, parents, skip + Workspace.PAGE);
                else if (label.startsWith("‹")) showFolder(folder, name, parents, skip - Workspace.PAGE);
                else {
                    Parent p = parents.pop();
                    showFolder(p.uri, p.name, parents, p.skip);
                }
            });
            dialog = b.show();
        });
    }

    void find() {
        if (workspace == null) { ui.pickFolder(); return; }
        AlertDialog.Builder b = ui.dialog();
        LinearLayout form = new LinearLayout(b.getContext());
        form.setOrientation(LinearLayout.VERTICAL);
        form.setPadding(ui.dp(20), 0, ui.dp(20), 0);
        EditText query = new EditText(b.getContext());
        query.setSingleLine();
        query.setHint("Search folder");
        query.setInputType(InputType.TYPE_CLASS_TEXT);
        form.addView(query);
        RadioGroup mode = new RadioGroup(b.getContext());
        RadioButton files = new RadioButton(b.getContext()), contents = new RadioButton(b.getContext());
        files.setText("File names and paths");
        contents.setText("Exact file contents");
        mode.addView(files); mode.addView(contents);
        files.setChecked(true);
        form.addView(mode);
        dialog = b.setTitle("Find in folder").setView(form).setNegativeButton("Cancel", null)
            .setPositiveButton("Find", (d, w) -> {
                String term = query.getText().toString();
                if (term.isEmpty()) return;
                boolean text = contents.isChecked();
                load("Searching folder", stop -> workspace.find(term, text, stop), result -> {
                    List<Workspace.Entry> hits = result.entries();
                    String title = hits.size() + (hits.size() == Workspace.LIMIT ? "+" : "") + " matches";
                    if (result.unreadable() > 0) title += " · " + result.unreadable() + " skipped";
                    AlertDialog.Builder matches = ui.dialog().setTitle(title).setNegativeButton("Close", null);
                    if (hits.isEmpty()) matches.setMessage("No matches found.");
                    else {
                        String[] labels = new String[hits.size()];
                        for (int i = 0; i < hits.size(); i++) {
                            Workspace.Entry e = hits.get(i);
                            labels[i] = e.path() + (text ? " · byte " + e.offset() : "");
                        }
                        matches.setItems(labels, (dd, i) -> {
                            Workspace.Entry e = hits.get(i);
                            ui.requestOpen(e.uri(), e.offset(), e.length(), null);
                        });
                    }
                    dialog = matches.show();
                });
            }).show();
    }

    @Override public void close() {
        if (current != null) current.cancel();
        if (dialog != null) dialog.dismiss();
        workers.shutdownNow();
    }
}
