package org.slate.editor;

import android.graphics.Typeface;
import android.os.*;
import android.text.*;
import android.view.*;
import android.view.inputmethod.InputMethodManager;
import android.content.Context;
import android.widget.*;
import java.util.*;
import java.util.concurrent.*;

final class SidebarSearch implements AutoCloseable {
    private final MainActivity ui;
    private final WorkspaceTree tree;
    private final Appearance theme;
    final LinearLayout controls;
    final TextView summary;
    final EditText input;
    final RadioButton files, contents;
    private final Button clear;
    private final ExecutorService scanner = Executors.newSingleThreadExecutor();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable submit = this::run;
    private final ArrayList<Workspace.Entry> hits = new ArrayList<>();
    final BaseAdapter adapter = new BaseAdapter() {
        @Override public int getCount() { return hits.size(); }
        @Override public Object getItem(int i) { return hits.get(i); }
        @Override public long getItemId(int i) { return i; }
        @Override public View getView(int i, View old, ViewGroup parent) {
            Workspace.Entry hit = hits.get(i);
            TextView label = old instanceof TextView ? (TextView)old : new TextView(ui);
            String location = hit.offset() >= 0 ? " · byte " + hit.offset() : "";
            label.setText(hit.path() + location);
            label.setContentDescription(hit.path() + location);
            label.setTextColor(theme.text());
            label.setTypeface(Typeface.DEFAULT, tree.isSelected(hit.uri()) ? Typeface.BOLD : Typeface.NORMAL);
            label.setBackgroundColor(tree.isSelected(hit.uri()) ? theme.background() : theme.surface());
            label.setMinHeight(ui.dp(44));
            label.setGravity(Gravity.CENTER_VERTICAL);
            label.setPadding(ui.dp(16), ui.dp(4), ui.dp(12), ui.dp(4));
            return label;
        }
    };
    private CancellationSignal pending;
    private boolean visible, closed;

    SidebarSearch(MainActivity ui, WorkspaceTree tree, Appearance theme) {
        this.ui = ui;
        this.tree = tree;
        this.theme = theme;
        controls = new LinearLayout(ui);
        controls.setPadding(ui.dp(10), 0, ui.dp(6), 0);
        controls.setOrientation(LinearLayout.VERTICAL);
        LinearLayout query = new LinearLayout(ui);
        query.setGravity(Gravity.CENTER_VERTICAL);
        input = new EditText(ui);
        input.setSingleLine(true);
        input.setHint("Search files");
        input.setContentDescription("Search files");
        query.addView(input, new LinearLayout.LayoutParams(0, ui.dp(48), 1));
        clear = new Button(ui, null, android.R.attr.borderlessButtonStyle);
        clear.setText("×");
        clear.setTextSize(21);
        clear.setContentDescription("Clear search");
        clear.setOnClickListener(v -> input.setText(""));
        query.addView(clear, ui.dp(40), ui.dp(48));
        controls.addView(query);
        RadioGroup modes = new RadioGroup(ui);
        modes.setOrientation(LinearLayout.HORIZONTAL);
        files = radio(modes, "Files", "Search file names");
        contents = radio(modes, "Contents", "Search file contents");
        files.setChecked(true);
        controls.addView(modes);
        summary = new TextView(ui);
        summary.setPadding(ui.dp(16), ui.dp(4), ui.dp(12), ui.dp(4));
        summary.setVisibility(View.GONE);
        clear.setVisibility(View.INVISIBLE);
        input.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) { restart(); }
            @Override public void afterTextChanged(Editable s) {}
        });
        modes.setOnCheckedChangeListener((g, id) -> restart());
    }

    private RadioButton radio(RadioGroup modes, String name, String description) {
        RadioButton b = new RadioButton(ui);
        b.setId(View.generateViewId());
        b.setText(name);
        b.setContentDescription(description);
        modes.addView(b, new RadioGroup.LayoutParams(0, ui.dp(42), 1));
        return b;
    }

    boolean active() { return input.length() > 0; }
    void focus() {
        input.requestFocus();
        ((InputMethodManager)ui.getSystemService(Context.INPUT_METHOD_SERVICE)).showSoftInput(input, InputMethodManager.SHOW_IMPLICIT);
    }
    void paint() {
        input.setTextColor(theme.text());
        input.setHintTextColor(theme.muted());
        clear.setTextColor(theme.accent());
        summary.setTextColor(theme.muted());
        for (RadioButton b : new RadioButton[]{files, contents}) {
            b.setTextColor(theme.text());
            b.setButtonTintList(android.content.res.ColorStateList.valueOf(theme.accent()));
        }
        adapter.notifyDataSetChanged();
    }

    void visible(boolean show) {
        if (visible == show) return;
        visible = show;
        if (active() && (pending != null || hits.isEmpty())) restart();
    }

    void restart() {
        handler.removeCallbacks(submit);
        if (pending != null) pending.cancel();
        pending = null;
        hits.clear();
        adapter.notifyDataSetChanged();
        boolean active = active();
        tree.displaySearch(active);
        clear.setVisibility(active ? View.VISIBLE : View.INVISIBLE);
        summary.setVisibility(active ? View.VISIBLE : View.GONE);
        if (active) {
            summary.setText("Searching…");
            if (visible && !closed) handler.postDelayed(submit, 200);
        }
    }

    private void run() {
        String term = input.getText().toString();
        boolean text = contents.isChecked();
        List<Workspace.Scope> folders = tree.searchFolders();
        List<Workspace.Entry> looseFiles = tree.searchFiles();
        CancellationSignal stop = new CancellationSignal();
        pending = stop;
        scanner.execute(() -> {
            try {
                Workspace.Results result = Workspace.find(ui.getContentResolver(), folders, looseFiles, term, text, stop);
                ui.runOnUiThread(() -> {
                    if (closed || pending != stop) return;
                    pending = null;
                    hits.addAll(result.entries());
                    int n = hits.size();
                    String message = n == 0 ? "No matches" : n + (n == Workspace.LIMIT ? "+" : "") + (n == 1 ? " match" : " matches");
                    if (result.unreadable() > 0) message += " · " + result.unreadable() + " skipped";
                    summary.setText(message);
                    adapter.notifyDataSetChanged();
                });
            } catch (RuntimeException error) {
                ui.runOnUiThread(() -> {
                    if (closed || pending != stop) return;
                    pending = null;
                    summary.setText("Search failed. " + error.getLocalizedMessage());
                });
            }
        });
    }

    void open(int index) {
        Workspace.Entry hit = hits.get(index);
        ui.openBrowserFile(hit.uri(), hit.offset(), hit.length());
    }

    @Override public void close() {
        closed = true;
        handler.removeCallbacks(submit);
        if (pending != null) pending.cancel();
        scanner.shutdownNow();
    }
}
