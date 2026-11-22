package org.slate.editor;

import android.app.AlertDialog;
import android.content.*;
import android.database.Cursor;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.CancellationSignal;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.text.TextUtils;
import android.view.*;
import android.widget.*;

import java.util.*;
import java.util.concurrent.*;

final class WorkspaceTree implements AutoCloseable {
    private final MainActivity ui;
    private final Appearance theme;
    private final SharedPreferences prefs;
    private final ExecutorService workers = Executors.newFixedThreadPool(2);
    private final Map<Uri, Node> grants = new LinkedHashMap<>();
    private final List<Row> rows = new ArrayList<>();
    private final TreeAdapter adapter = new TreeAdapter();
    private final LinearLayout pane;
    private final ListView list;
    private final TextView title, empty;
    private final Button openFolder, openFile, newFile, refresh;
    private final SidebarSearch search;
    private int treeScroll, treeTop;
    private Uri selected;
    private boolean visible, closed;
    private int grantGeneration;

    private static final class Node {
        final Workspace workspace;
        final Uri uri;
        final boolean folder;
        final Node parent;
        String name;
        boolean expanded, more, failed, named;
        int skip;
        List<Node> children;
        CancellationSignal request;
        Node(Node parent, Workspace workspace, Uri uri, String name, boolean folder) {
            this.parent = parent;
            this.workspace = workspace;
            this.uri = uri;
            this.name = name;
            this.folder = folder;
        }
    }
    private enum Kind { ITEM, LOAD, EMPTY, RETRY, NEXT, PREVIOUS, FILES }
    private record Row(Node node, Kind kind, int depth) {}

    WorkspaceTree(MainActivity ui, Appearance theme) {
        this.ui = ui;
        this.theme = theme;
        prefs = ui.getPreferences(Context.MODE_PRIVATE);
        pane = new LinearLayout(ui);
        pane.setOrientation(LinearLayout.VERTICAL);
        pane.setClickable(true);
        pane.setFocusableInTouchMode(true);
        pane.setContentDescription("Files pane");
        title = new TextView(ui);
        title.setText("Files");
        title.setTextSize(20);
        title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        title.setGravity(Gravity.CENTER_VERTICAL);
        title.setPadding(ui.dp(16), 0, 0, 0);
        LinearLayout heading = new LinearLayout(ui);
        heading.setGravity(Gravity.CENTER_VERTICAL);
        heading.addView(title, new LinearLayout.LayoutParams(0, ui.dp(48), 1));
        refresh = new Button(ui, null, android.R.attr.borderlessButtonStyle);
        refresh.setText("Refresh");
        refresh.setContentDescription("Refresh files");
        refresh.setAllCaps(false);
        refresh.setOnClickListener(v -> refresh());
        heading.addView(refresh, -2, ui.dp(48));
        pane.addView(heading);
        LinearLayout actions = new LinearLayout(ui);
        openFolder = action(actions, "Open folder", ui::pickFolder);
        openFile = action(actions, "Open file", ui::pickFile);
        newFile = action(actions, "New file", this::newFile);
        pane.addView(actions);
        search = new SidebarSearch(ui, this, theme);
        pane.addView(search.controls);
        pane.addView(search.summary);
        empty = new TextView(ui);
        empty.setText("Open a folder to browse its files here.");
        empty.setPadding(ui.dp(16), ui.dp(12), ui.dp(16), ui.dp(12));
        pane.addView(empty);
        list = new ListView(ui);
        list.setContentDescription("Workspace files");
        list.setDivider(null);
        list.setAdapter(adapter);
        list.setOnItemClickListener((p, v, i, id) -> {
            if (search.active()) search.open(i); else click(rows.get(i));
        });
        pane.addView(list, new LinearLayout.LayoutParams(-1, 0, 1));
        paint();
        readGrants(false);
    }

    private Button action(LinearLayout bar, String text, Runnable callback) {
        Button button = new Button(ui, null, android.R.attr.borderlessButtonStyle);
        button.setText(text);
        button.setMinWidth(0);
        button.setMinimumWidth(0);
        button.setPadding(0, 0, 0, 0);
        button.setContentDescription(text);
        button.setAllCaps(false);
        button.setOnClickListener(v -> callback.run());
        bar.addView(button, new LinearLayout.LayoutParams(0, ui.dp(48), 1));
        return button;
    }

    View view() { return pane; }
    boolean hasFolder() { return grants.values().stream().anyMatch(n -> n.folder); }
    List<Workspace.Scope> searchFolders() {
        ArrayList<Workspace.Scope> roots = new ArrayList<>();
        for (Node n : grants.values()) if (n.folder) roots.add(new Workspace.Scope(n.workspace, n.name));
        return roots;
    }
    List<Workspace.Entry> searchFiles() {
        ArrayList<Workspace.Entry> files = new ArrayList<>();
        for (Node n : grants.values()) if (!n.folder) files.add(new Workspace.Entry(n.name, n.name, n.uri, false, -1, 0));
        return files;
    }

    void focusSearch() { search.focus(); }
    void focusSearch(boolean contents) {
        (contents ? search.contents : search.files).setChecked(true);
        search.focus();
    }

    void displaySearch(boolean active) {
        empty.setVisibility(!active && rows.isEmpty() ? View.VISIBLE : View.GONE);
        if (active && list.getAdapter() != search.adapter) {
            treeScroll = list.getFirstVisiblePosition();
            treeTop = list.getChildCount() == 0 ? 0 : list.getChildAt(0).getTop();
            list.setAdapter(search.adapter);
            list.setContentDescription("Search results");
        } else if (!active && list.getAdapter() != adapter) {
            list.setAdapter(adapter);
            list.setContentDescription("Workspace files");
            list.setSelectionFromTop(treeScroll, treeTop);
        }
    }

    void visible(boolean show) {
        visible = show;
        search.visible(show);
        if (show) adapter.notifyDataSetChanged();
    }

    private static boolean wholeTree(Uri grant) {
        return DocumentsContract.isTreeUri(grant) && grant.getPathSegments().size() == 2;
    }

    private void readGrants(boolean refreshRows) {
        int generation = ++grantGeneration;
        workers.execute(() -> {
            List<UriPermission> permissions = ui.getContentResolver().getPersistedUriPermissions();
            ui.runOnUiThread(() -> {
                if (closed || generation != grantGeneration) return;
                Set<Uri> valid = new HashSet<>();
                for (UriPermission permission : permissions) {
                    if (!permission.isReadPermission()) continue;
                    Uri uri = permission.getUri();
                    valid.add(uri);
                    add(uri, wholeTree(uri), null);
                }
                if (refreshRows) {
                    // A temporary Open With grant may still be live even if it
                    // never appeared in the provider's persisted grant table.
                    grants.entrySet().removeIf(e -> {
                        boolean gone = !valid.contains(e.getKey()) && ui.checkCallingOrSelfUriPermission(
                            e.getKey(), Intent.FLAG_GRANT_READ_URI_PERMISSION) != android.content.pm.PackageManager.PERMISSION_GRANTED;
                        if (gone) release(e.getValue());
                        return gone;
                    });
                }
                rebuild();
                if (search.active()) search.restart();
            });
        });
    }

    private static String fallbackName(Uri uri, boolean folder) {
        String id = wholeTree(uri) ? DocumentsContract.getTreeDocumentId(uri) : uri.getLastPathSegment();
        if (id == null || id.isEmpty()) return folder ? "Folder" : "File";
        int last = Math.max(id.lastIndexOf('/'), id.lastIndexOf(':'));
        return last >= 0 && last + 1 < id.length() ? id.substring(last + 1) : id;
    }

    private Node add(Uri grant, boolean folder, String name) {
        Node known = grants.get(grant);
        if (known != null) {
            if (name != null) { known.name = name; known.named = true; }
            return known;
        }
        Workspace ws = folder ? new Workspace(ui.getContentResolver(), grant) : null;
        Uri document = folder ? ws.root : grant;
        String cached = prefs.getString("label/" + grant, null);
        Node n = node(null, ws, document, name != null ? name : cached != null ? cached : fallbackName(grant, folder), folder);
        n.named = name != null || cached != null;
        grants.put(grant, n);
        return n;
    }

    private Node node(Node parent, Workspace workspace, Uri uri, String name, boolean folder) {
        Node n = new Node(parent, workspace, uri, name, folder);
        if (folder) {
            n.expanded = prefs.getBoolean("expanded/" + uri, false);
            n.skip = prefs.getInt("page/" + uri, 0);
        }
        return n;
    }

    void choose(Uri tree) {
        search.input.setText("");
        Node root = add(tree, true, null);
        root.expanded = true;
        savePosition(root);
        rebuild();
        for (int i = 0; i < rows.size(); i++) if (rows.get(i).node == root) {
            list.setSelection(i);
            break;
        }
    }

    void openedFile(Uri uri, String name) {
        add(uri, false, name);
        prefs.edit().putString("label/" + uri, name).apply();
        rebuild();
        if (search.active()) search.restart();
    }

    void select(Uri uri) {
        if (!Objects.equals(selected, uri)) {
            selected = uri;
            adapter.notifyDataSetChanged();
            search.adapter.notifyDataSetChanged();
        }
    }

    void paint() {
        pane.setBackgroundColor(theme.surface());
        title.setTextColor(theme.text());
        empty.setTextColor(theme.muted());
        search.paint();
        for (Button b : new Button[]{openFolder, openFile, newFile, refresh}) b.setTextColor(theme.accent());
        adapter.notifyDataSetChanged();
    }

    void newFile() {
        ArrayList<Node> roots = new ArrayList<>();
        for (Node n : grants.values()) if (n.folder) roots.add(n);
        if (roots.isEmpty()) { ui.pickNewParent(); return; }
        String[] names = new String[roots.size() + 1];
        for (int i = 0; i < roots.size(); i++) {
            Node n = roots.get(i);
            names[i] = n.name;
            for (Node other : roots) if (other != n && n.name.equals(other.name)) {
                names[i] += " · " + DocumentsContract.getDocumentId(n.uri);
                break;
            }
        }
        names[roots.size()] = "Choose another folder…";
        ui.dialog().setTitle("Create file in").setItems(names, (d, i) -> {
            if (i == roots.size()) ui.pickNewParent(); else askName(roots.get(i));
        }).setNegativeButton("Cancel", null).show();
    }

    void newAtRoot(Uri tree) {
        Node n = grants.get(tree);
        if (n != null) askName(n);
    }

    private String location(Node node) {
        Deque<String> parts = new ArrayDeque<>();
        for (Node n = node; n != null; n = n.parent) parts.addFirst(n.name);
        return String.join("/", parts);
    }

    private void askName(Node parent) {
        AlertDialog.Builder builder = ui.dialog();
        EditText name = new EditText(builder.getContext());
        name.setSingleLine();
        name.setHint("File name");
        name.setContentDescription("New file name");
        LinearLayout field = new LinearLayout(builder.getContext());
        field.setPadding(ui.dp(20), 0, ui.dp(20), 0);
        field.addView(name, -1, -2);
        AlertDialog prompt = builder.setTitle("New file").setMessage("Create in " + location(parent))
            .setView(field).setPositiveButton("Create", null).setNegativeButton("Cancel", null).create();
        prompt.setOnShowListener(d -> prompt.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            String value = name.getText().toString();
            String issue = Workspace.invalidName(value);
            if (issue != null) { name.setError(issue); return; }
            prompt.dismiss();
            ui.createFile(parent.workspace, parent.uri, value);
        }));
        prompt.show();
    }

    private void actions(View anchor, Node target) {
        PopupMenu menu = new PopupMenu(ui, anchor);
        if (target.folder) menu.getMenu().add("New file here…")
            .setOnMenuItemClickListener(item -> { askName(target); return true; });
        else menu.getMenu().add("Delete file…")
            .setOnMenuItemClickListener(item -> { ui.deleteFile(target.uri, target.name); return true; });
        menu.show();
    }

    void created(Uri parent, int pageStart) {
        for (Row r : rows) if (r.kind == Kind.ITEM && r.node.folder && sameDocument(r.node.uri, parent)) {
            r.node.expanded = true;
            if (pageStart >= 0) r.node.skip = pageStart;
            savePosition(r.node);
            release(r.node);
        }
        if (search.active()) search.input.setText("");
        rebuild();
    }

    void deleted(Uri target) {
        grantGeneration++;
        grants.entrySet().removeIf(e -> !e.getValue().folder && sameDocument(e.getValue().uri, target));
        for (Node n : grants.values()) if (n.folder) release(n);
        for (UriPermission p : ui.getContentResolver().getPersistedUriPermissions()) {
            if (wholeTree(p.getUri()) || !sameDocument(p.getUri(), target)) continue;
            int flags = (p.isReadPermission() ? Intent.FLAG_GRANT_READ_URI_PERMISSION : 0)
                | (p.isWritePermission() ? Intent.FLAG_GRANT_WRITE_URI_PERMISSION : 0);
            try { ui.getContentResolver().releasePersistableUriPermission(p.getUri(), flags); }
            catch (SecurityException ignored) { /* The provider may revoke the grant on deletion. */ }
        }
        rebuild();
        if (search.active()) search.restart();
    }

    private void savePosition(Node n) {
        prefs.edit().putBoolean("expanded/" + n.uri, n.expanded).putInt("page/" + n.uri, n.skip).apply();
    }

    private void rebuild() {
        rows.clear();
        for (Node root : grants.values()) if (root.folder) append(root, 0);
        boolean fileSection = false;
        for (Node root : grants.values()) if (!root.folder) {
            if (!fileSection) { rows.add(new Row(null, Kind.FILES, 0)); fileSection = true; }
            append(root, 0);
        }
        empty.setVisibility(rows.isEmpty() && !search.active() ? View.VISIBLE : View.GONE);
        adapter.notifyDataSetChanged();
    }

    private void append(Node node, int depth) {
        rows.add(new Row(node, Kind.ITEM, depth));
        if (!node.expanded) return;
        if (node.children == null) {
            rows.add(new Row(node, node.failed ? Kind.RETRY : Kind.LOAD, depth + 1));
            return;
        }
        if (node.skip > 0) rows.add(new Row(node, Kind.PREVIOUS, depth + 1));
        if (node.children.isEmpty()) rows.add(new Row(node, Kind.EMPTY, depth + 1));
        for (Node child : node.children) append(child, depth + 1);
        if (node.more) rows.add(new Row(node, Kind.NEXT, depth + 1));
    }

    private static void release(Node n) {
        if (n.request != null) n.request.cancel();
        n.request = null;
        if (n.children != null) for (Node c : n.children) release(c);
        n.children = null;
        n.failed = false;
    }

    private void refresh() {
        for (Node root : grants.values()) {
            release(root);
            root.named = false;
        }
        rebuild();
        readGrants(true);
    }

    private void click(Row row) {
        Node n = row.node;
        switch (row.kind) {
        case ITEM -> {
            if (!n.folder) { ui.openBrowserFile(n.uri, -1, 0); return; }
            n.expanded = !n.expanded;
            if (!n.expanded) release(n);
            savePosition(n);
        }
        case RETRY -> n.failed = false;
        case NEXT, PREVIOUS -> {
            release(n);
            n.skip += row.kind == Kind.NEXT ? Workspace.PAGE : -Workspace.PAGE;
            savePosition(n);
        }
        default -> { return; }
        }
        rebuild();
    }

    private boolean ancestor(Node parent, Uri uri) {
        for (Node n = parent; n != null; n = n.parent) if (n.uri.equals(uri)) return true;
        return false;
    }

    private void load(Node parent) {
        if (!visible || parent.request != null || parent.failed || closed) return;
        CancellationSignal request = new CancellationSignal();
        parent.request = request;
        int skip = parent.skip;
        workers.execute(() -> {
            try {
                Workspace.Listing page = parent.workspace.list(parent.uri, skip, request);
                ui.runOnUiThread(() -> {
                    if (closed || parent.request != request) return;
                    parent.request = null;
                    parent.children = new ArrayList<>(page.entries().size());
                    for (Workspace.Entry e : page.entries()) {
                        if (e.directory() && ancestor(parent, e.uri())) continue;
                        parent.children.add(node(parent, parent.workspace, e.uri(), e.name(), e.directory()));
                    }
                    parent.more = page.more();
                    rebuild();
                });
            } catch (Exception e) {
                ui.runOnUiThread(() -> {
                    if (closed || parent.request != request) return;
                    parent.request = null;
                    parent.failed = true;
                    rebuild();
                });
            }
        });
    }

    private void lookupName(Node n) {
        if (n.named || !visible || closed) return;
        n.named = true;
        Uri key = n.folder ? n.workspace.tree : n.uri;
        workers.execute(() -> {
            try (Cursor cursor = ui.getContentResolver().query(n.uri, new String[]{Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
                if (cursor == null || !cursor.moveToFirst() || cursor.isNull(0)) return;
                String name = cursor.getString(0);
                ui.runOnUiThread(() -> {
                    if (closed) return;
                    n.name = name;
                    prefs.edit().putString("label/" + key, name).apply();
                    adapter.notifyDataSetChanged();
                });
            } catch (RuntimeException ignored) { /* The URI supplies a fallback label. */ }
        });
    }

    boolean isSelected(Uri uri) { return sameDocument(uri, selected); }

    private static boolean sameDocument(Uri first, Uri second) {
        if (Objects.equals(first, second)) return true;
        if (first == null || second == null || !Objects.equals(first.getAuthority(), second.getAuthority())) return false;
        List<String> a = first.getPathSegments(), b = second.getPathSegments();
        int i = a.indexOf("document"), j = b.indexOf("document");
        return i >= 0 && j >= 0 && i + 1 < a.size() && j + 1 < b.size() && a.get(i + 1).equals(b.get(j + 1));
    }

    private final class TreeAdapter extends BaseAdapter {
        @Override public int getCount() { return rows.size(); }
        @Override public Row getItem(int index) { return rows.get(index); }
        @Override public long getItemId(int index) { return index; }
        @Override public boolean areAllItemsEnabled() { return false; }
        @Override public boolean isEnabled(int i) {
            Kind k = rows.get(i).kind;
            return k == Kind.ITEM || k == Kind.RETRY || k == Kind.NEXT || k == Kind.PREVIOUS;
        }

        @Override public View getView(int index, View old, ViewGroup listParent) {
            Row row = getItem(index);
            RowView view = old instanceof RowView reused ? reused : new RowView();
            Node n = row.node;
            if (row.kind == Kind.ITEM && row.depth == 0) lookupName(n);
            if (row.kind == Kind.LOAD) load(n);
            boolean item = row.kind == Kind.ITEM;
            boolean active = item && !n.folder && sameDocument(n.uri, selected);
            String text = switch (row.kind) {
                case ITEM -> n.name;
                case FILES -> "Opened files";
                case LOAD -> "Loading…";
                case EMPTY -> "Empty folder";
                case RETRY -> "Couldn’t read folder · Retry";
                case NEXT -> "Next files ›";
                case PREVIOUS -> "‹ Previous files";
            };
            view.setPadding(ui.dp(8 + Math.min(9, row.depth) * 13), 0, ui.dp(6), 0);
            view.setBackgroundColor(active ? theme.background() : theme.surface());
            view.arrow.setText(item && n.folder ? n.expanded ? "▾" : "▸" : "");
            view.arrow.setTextColor(theme.muted());
            view.icon.setVisibility(item ? View.VISIBLE : View.GONE);
            if (item) {
                view.icon.setImageResource(n.folder ? R.drawable.tree_folder : R.drawable.tree_file);
                view.icon.setColorFilter(theme.muted());
            }
            view.label.setText(text);
            view.label.setTextColor(item ? theme.text() : theme.muted());
            view.label.setTypeface(Typeface.DEFAULT, active || row.kind == Kind.FILES ? Typeface.BOLD : Typeface.NORMAL);
            view.actions.setVisibility(item ? View.VISIBLE : View.GONE);
            if (item) {
                view.actions.setTextColor(theme.muted());
                view.actions.setContentDescription("Actions for " + n.name);
                view.actions.setOnClickListener(v -> actions(v, n));
            }
            view.setSelected(active);
            view.setContentDescription(text + (item && n.folder ? n.expanded ? ", expanded folder" : ", collapsed folder" : active ? ", current file" : ""));
            return view;
        }
    }

    private final class RowView extends LinearLayout {
        final TextView arrow, label;
        final Button actions;
        final ImageView icon;
        RowView() {
            super(ui);
            setGravity(Gravity.CENTER_VERTICAL);
            setMinimumHeight(ui.dp(44));
            arrow = new TextView(ui);
            arrow.setGravity(Gravity.CENTER);
            addView(arrow, ui.dp(16), -2);
            icon = new ImageView(ui);
            addView(icon, ui.dp(20), ui.dp(20));
            label = new TextView(ui);
            label.setSingleLine();
            label.setEllipsize(TextUtils.TruncateAt.END);
            label.setPadding(ui.dp(6), 0, 0, 0);
            addView(label, new LinearLayout.LayoutParams(0, -2, 1));
            actions = new Button(ui, null, android.R.attr.borderlessButtonStyle);
            actions.setText("⋮");
            actions.setTextSize(20);
            actions.setFocusable(false);
            actions.setPadding(0, 0, 0, 0);
            addView(actions, ui.dp(44), ui.dp(44));
        }
    }

    @Override public void close() {
        closed = true;
        search.close();
        for (Node root : grants.values()) release(root);
        workers.shutdownNow();
    }
}
