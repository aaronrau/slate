package org.slate.editor;

import android.app.*;
import android.content.*;
import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.*;
import android.text.*;
import android.text.method.*;
import android.view.*;
import android.view.inputmethod.InputMethodManager;
import android.widget.*;

import java.nio.*;
import java.nio.charset.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.Consumer;
import static java.nio.charset.StandardCharsets.UTF_8;

@SuppressWarnings("deprecation")
public final class MainActivity extends Activity {
    private static final int OPEN = 1, FOLDER = 2, CREATE = 3;
    private static final int SOURCE_PAGE = 32 * 1024, PREVIEW_PAGE = 128 * 1024;
    private static final int HISTORY_BYTES = 8 * 1024 * 1024;
    private final ExecutorService io = Executors.newSingleThreadExecutor();
    private EditorFile file;
    private Appearance theme;
    private Navigation navigation;
    private WorkspaceBrowser workspace;
    private Runnable afterCreate;
    private boolean busy, previewing, loading, readable, wholeSelection;
    private long start, end;
    private String pageText = "";
    private long currentTab;
    private LinearLayout root, toolbar, tabBar, footer;
    private FrameLayout body;
    private Gallery tabs;
    private Button back, forward, mode, save, files, more, previous, next;
    private TextView brand, status;
    private SourceEditor editor;
    private PreviewPane preview;
    private SeekBar seek;
    private TabAdapter tabAdapter;
    private final ArrayDeque<Change> undo = new ArrayDeque<>(), redo = new ArrayDeque<>();
    private int undoBytes;
    private record Change(long offset, byte[] before, byte[] after) {
        int cost() { return before.length + after.length + 64; }
    }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        theme = new Appearance(this);
        setTheme(theme.dark ? android.R.style.Theme_Material_NoActionBar : android.R.style.Theme_Material_Light_NoActionBar);
        file = new EditorFile();
        navigation = new Navigation(this);
        workspace = new WorkspaceBrowser(this);
        build();
        paint();
        showPage(0, 0);
        Uri launch = getIntent().getData();
        Navigation.Place last = navigation.relative(0);
        if (state == null && launch != null && "content".equals(launch.getScheme())) {
            persist(launch, getIntent());
            requestOpen(launch, -1, 0, null);
        } else if ((state == null || !state.getBoolean("untitled")) && last != null) {
            followHistory(last, -1);
        }
    }

    int dp(int units) { return Math.round(getResources().getDisplayMetrics().density * units); }
    AlertDialog.Builder dialog() { return new AlertDialog.Builder(this, theme.dialogTheme()); }
    private LinearLayout row() {
        LinearLayout result = new LinearLayout(this);
        result.setGravity(Gravity.CENTER_VERTICAL);
        return result;
    }
    private Button button(LinearLayout row, String text, String label, Runnable action) {
        Button b = new Button(this, null, android.R.attr.borderlessButtonStyle);
        b.setText(text);
        b.setContentDescription(label);
        b.setAllCaps(false);
        b.setMinWidth(dp(40)); b.setMinimumWidth(dp(40));
        b.setPadding(dp(8), 0, dp(8), 0);
        b.setOnClickListener(v -> { if (!busy) action.run(); });
        row.addView(b, -2, dp(48));
        return b;
    }
    private LinearLayout.LayoutParams flex() { return new LinearLayout.LayoutParams(0, -2, 1); }

    private void build() {
        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setFocusableInTouchMode(true);
        setContentView(root);
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            root.setOnApplyWindowInsetsListener((v, insets) -> {
                android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars()
                    | WindowInsets.Type.displayCutout() | WindowInsets.Type.ime());
                v.setPadding(bars.left, bars.top, bars.right, bars.bottom);
                return insets;
            });
        }
        toolbar = row();
        toolbar.setPadding(dp(8), 0, 0, 0);
        root.addView(toolbar);
        ImageView icon = new ImageView(this);
        icon.setImageResource(R.drawable.slate_icon);
        toolbar.addView(icon, dp(30), dp(30));
        brand = new TextView(this);
        brand.setText("Slate");
        brand.setTextSize(19);
        brand.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        brand.setPadding(dp(6), 0, 0, 0);
        brand.setSingleLine();
        toolbar.addView(brand, flex());
        files = button(toolbar, "Files", "Browse files", () -> {
            if (workspace.hasFolder()) workspace.browse(); else pickFile();
        });
        mode = button(toolbar, "Preview", "Switch Markdown view", this::togglePreview);
        save = button(toolbar, "Save", "Save file", () -> save(false, null));
        more = button(toolbar, "⋮", "More options", this::menu);
        more.setTextSize(22);

        tabBar = row();
        root.addView(tabBar);
        back = button(tabBar, "‹", "Previous file", () -> travel(-1));
        back.setTextSize(24);
        tabs = new Gallery(this);
        tabs.setGravity(Gravity.CENTER_VERTICAL);
        tabs.setUnselectedAlpha(1f);
        tabs.setSpacing(dp(2));
        tabAdapter = new TabAdapter();
        tabs.setAdapter(tabAdapter);
        tabs.setOnItemClickListener((p, v, index, id) -> {
            if (busy) return;
            Navigation.Place place = navigation.tab(id);
            if (place != null && !place.uri().equals(file.uri))
                requestOpen(place.uri(), place.offset(), 0, place);
        });
        tabBar.addView(tabs, new LinearLayout.LayoutParams(0, dp(42), 1));
        forward = button(tabBar, "›", "Next file", () -> travel(1));
        forward.setTextSize(24);

        body = new FrameLayout(this);
        root.addView(body, new LinearLayout.LayoutParams(-1, 0, 1));
        editor = new SourceEditor();
        editor.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE
            | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        editor.setTypeface(Typeface.MONOSPACE);
        editor.setTextSize(15);
        editor.setBackgroundColor(theme.background());
        editor.setGravity(Gravity.TOP | Gravity.START);
        editor.setPadding(dp(16), dp(12), dp(16), dp(12));
        editor.setVerticalScrollBarEnabled(true);
        editor.setHorizontallyScrolling(false);
        editor.setSaveEnabled(false);
        editor.setHint("Write here…");
        editor.setFilters(new InputFilter[]{(source, from, to, dest, dstart, dend) -> {
            if (!loading && to - from > 1024 * 1024) {
                toast("Paste up to 1 MiB of text at a time.");
                return dest.subSequence(dstart, dend);
            }
            return null;
        }});
        editor.addTextChangedListener(new TextWatcher() {
            int changedAt, removed, inserted;
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {
                changedAt = start;
                removed = count;
                inserted = after;
            }
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
            @Override public void afterTextChanged(Editable s) {
                if (!loading && !busy) applyText(s.toString(), changedAt, removed, inserted);
            }
        });
        body.addView(editor, -1, -1);
        footer = row();
        footer.setPadding(dp(6), 0, dp(6), 0);
        root.addView(footer);
        previous = button(footer, "‹", "Previous page", () -> showPage(Math.max(0, start - (previewing ? PREVIEW_PAGE : SOURCE_PAGE)), 0, true));
        status = new TextView(this);
        status.setTextSize(12);
        status.setPadding(dp(6), 0, dp(6), 0);
        footer.addView(status, flex());
        seek = new SeekBar(this);
        seek.setContentDescription("Document position");
        seek.setMax(10000);
        footer.addView(seek, new LinearLayout.LayoutParams(dp(96), dp(36)));
        seek.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar, int progress, boolean user) {}
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {
                if (!busy) showPage(Math.round((bar.getProgress() / 10000.0) * file.document.size()), 0);
            }
        });
        next = button(footer, "›", "Next page", () -> showPage(end, 0, true));
    }

    private final class TabAdapter extends BaseAdapter {
        private int count;
        void refresh() {
            count = navigation.tabCount();
            notifyDataSetChanged();
        }
        @Override public int getCount() { return count; }
        @Override public Navigation.Place getItem(int index) { return navigation.tab((long)index + 1); }
        @Override public long getItemId(int index) { return (long)index + 1; }
        @Override public boolean hasStableIds() { return true; }
        @Override public View getView(int index, View reuse, ViewGroup parent) {
            TextView label = reuse instanceof TextView ? (TextView)reuse : new TextView(MainActivity.this);
            Navigation.Place place = getItem(index);
            boolean active = place.tab() == currentTab;
            label.setText(place.name());
            label.setContentDescription(place.name() + (active ? ", current tab" : ""));
            label.setTextColor(active ? theme.text() : theme.muted());
            label.setTypeface(Typeface.DEFAULT, active ? Typeface.BOLD : Typeface.NORMAL);
            label.setBackgroundColor(active ? theme.background() : theme.surface());
            label.setGravity(Gravity.CENTER);
            label.setSingleLine();
            label.setEllipsize(TextUtils.TruncateAt.END);
            label.setMaxWidth(dp(190));
            label.setPadding(dp(14), 0, dp(14), 0);
            label.setLayoutParams(new Gallery.LayoutParams(-2, dp(38)));
            return label;
        }
    }

    private void updateTabs() {
        Navigation.Place place = navigation.relative(0);
        currentTab = file.uri != null && place != null && file.uri.equals(place.uri()) ? place.tab() : 0;
        tabAdapter.refresh();
        if (currentTab > 0) tabs.setSelection((int)(currentTab - 1), false);
        tabBar.setVisibility(tabAdapter.getCount() > 0 ? View.VISIBLE : View.GONE);
        back.setEnabled(!busy && navigation.relative(currentTab == 0 ? 0 : -1) != null);
        forward.setEnabled(!busy && currentTab != 0 && navigation.relative(1) != null);
    }

    private void travel(int delta) {
        Navigation.Place p = navigation.relative(currentTab == 0 && delta < 0 ? 0 : delta);
        if (p != null) followHistory(p, delta);
    }

    private record HistoryTarget(EditorFile file, Navigation.Place place) {}

    private void followHistory(Navigation.Place place, int direction) {
        leave(() -> {
            remember();
            runIO(() -> {
                Navigation.Place target = place;
                while (target != null && !isDestroyed()) {
                    try {
                        return new HistoryTarget(EditorFile.open(this, target.uri()), target);
                    } catch (java.io.FileNotFoundException | SecurityException gone) {
                        navigation.unavailable(target.tab());
                        target = navigation.from(target.visit(), direction);
                    }
                }
                return null;
            }, result -> {
                if (result == null) { toast("The file is no longer available."); return; }
                Navigation.Place target = result.place();
                install(result.file(), target.offset(), 0, target);
            });
        });
    }

    private void paint() {
        root.setBackgroundColor(theme.surface());
        body.setBackgroundColor(theme.background());
        editor.setBackgroundColor(theme.background());
        editor.setTextColor(theme.text());
        editor.setHintTextColor(theme.muted());
        editor.setHighlightColor((theme.accent() & 0x00ffffff) | 0x66000000);
        status.setTextColor(theme.muted());
        brand.setTextColor(theme.text());
        seek.setProgressTintList(ColorStateList.valueOf(theme.accent()));
        seek.setThumbTintList(ColorStateList.valueOf(theme.accent()));
        for (Button b : new Button[]{back, forward, files, mode, save, more, previous, next}) {
            b.setTextColor(new ColorStateList(new int[][]{{-android.R.attr.state_enabled}, {}},
                new int[]{(theme.muted() & 0xffffff) | 0x65000000, theme.accent()}));
        }
        View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(theme.dark ? 0 : View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR);
        getWindow().setStatusBarColor(theme.surface());
        getWindow().setNavigationBarColor(theme.surface());
        updateTabs();
        if (preview != null) preview.render(pageText, theme);
    }

    private boolean markdown() {
        String n = file.name.toLowerCase(Locale.ROOT);
        return n.endsWith(".md") || n.endsWith(".markdown") || n.endsWith(".mdown");
    }
    private long caret() {
        if (previewing) return start;
        int point = Math.max(0, Math.min(editor.getSelectionStart(), pageText.length()));
        return start + pageText.substring(0, point).getBytes(UTF_8).length;
    }
    private long boundary(long target) {
        long b = file.document.boundary(target);
        if (b < 0) throw new IllegalStateException("Unable to read page boundary");
        return b;
    }

    private void showPage(long offset, int selectedBytes) {
        showPage(offset, selectedBytes, false);
    }

    private void showPage(long offset, int selectedBytes, boolean atPageStart) {
        long size = file.document.size();
        long point = Math.max(0, Math.min(size, offset));
        int unit = previewing ? PREVIEW_PAGE : SOURCE_PAGE;
        long page = Math.min(point, Math.max(0, size - 1)) / unit;
        try {
            long nominal = atPageStart ? point : page * unit;
            long first = boundary(nominal);
            if (!atPageStart && first > point && page > 0) { nominal -= unit; first = boundary(nominal); }
            long last = boundary(size - nominal <= unit ? size : nominal + unit);
            byte[] bytes = file.document.read(first, (int)(last - first));
            String text = new String(bytes, UTF_8);
            readable = Arrays.equals(bytes, text.getBytes(UTF_8));
            loading = true;
            wholeSelection = false;
            start = first; end = last; pageText = text;
            if (previewing) {
                if (preview == null) {
                    ((InputMethodManager)getSystemService(INPUT_METHOD_SERVICE)).hideSoftInputFromWindow(editor.getWindowToken(), 0);
                    editor.setVisibility(View.GONE);
                    editor.setText("");
                    preview = new PreviewPane(this);
                    body.addView(preview, -1, -1);
                }
                preview.render(text, theme);
            } else {
                removePreview();
                editor.setVisibility(View.VISIBLE);
                editor.setText(text);
                int cursorBytes = (int)Math.max(0, Math.min(bytes.length, point - first));
                int selectionEnd = (int)Math.min(bytes.length, (long)cursorBytes + selectedBytes);
                int a = new String(bytes, 0, cursorBytes, UTF_8).length();
                int b = new String(bytes, 0, selectionEnd, UTF_8).length();
                editor.setSelection(Math.min(a, text.length()), Math.min(b, text.length()));
                editor.scrollTo(0, 0);
                editor.setFocusableInTouchMode(readable);
                editor.setCursorVisible(readable);
            }
        } catch (Exception e) {
            readable = false;
            error(e);
        } finally {
            loading = false;
            refresh();
        }
    }

    private void removePreview() {
        if (preview != null) {
            body.removeView(preview);
            preview.destroy();
            preview = null;
        }
    }

    private void refresh() {
        long size = file.document.size();
        boolean pages = start != 0 || end < size;
        status.setText((file.dirty ? "• " : "") + (currentTab == 0 || !pages ? file.name + "  ·  " : "")
            + (wholeSelection ? "All bytes selected" : pages ? start + "–" + end + " of " + size : size + " bytes")
            + (!readable ? "  ·  Invalid UTF-8 (read only)" : ""));
        mode.setVisibility(markdown() ? View.VISIBLE : View.GONE);
        mode.setText(previewing ? "Source" : "Preview");
        previous.setVisibility(pages ? View.VISIBLE : View.GONE);
        next.setVisibility(pages ? View.VISIBLE : View.GONE);
        seek.setVisibility(pages ? View.VISIBLE : View.GONE);
        previous.setEnabled(!busy && start > 0);
        next.setEnabled(!busy && end < size);
        seek.setProgress(size == 0 ? 0 : (int)Math.round(10000.0 * start / size));
        editor.setEnabled(!busy && readable);
        save.setEnabled(!busy && (file.dirty || file.uri == null));
        mode.setEnabled(!busy);
        files.setEnabled(!busy);
        more.setEnabled(!busy);
        seek.setEnabled(!busy);
    }

    private void togglePreview() {
        long offset = caret();
        previewing = !previewing;
        showPage(offset, 0);
    }

    private void applyText(String updated, int begin, int oldCount, int newCount) {
        if (pageText.equals(updated)) return;
        int a = begin + oldCount, b = begin + newCount;
        if (wholeSelection) {
            begin = 0;
            a = pageText.length();
            b = updated.length();
        } else {
            // Some input methods modify one half of a surrogate pair. Include
            // the unchanged half when calculating the corresponding UTF-8 edit.
            if (begin > 0 && Character.isHighSurrogate(pageText.charAt(begin - 1))) begin--;
            if (a < pageText.length() && Character.isLowSurrogate(pageText.charAt(a))) { a++; b++; }
        }
        long offset = start + pageText.substring(0, begin).getBytes(UTF_8).length;
        byte[] erased = pageText.substring(begin, a).getBytes(UTF_8);
        byte[] added = updated.substring(begin, b).getBytes(UTF_8);
        long removed = erased.length;
        boolean global = wholeSelection;
        if (global) { offset = 0; removed = file.document.size(); }
        try {
            if (global && removed < HISTORY_BYTES) erased = file.document.read(0, (int)removed);
            file.document.replace(offset, removed, added);
            record(offset, removed, erased, added);
            file.dirty = true;
            end += added.length - removed;
            pageText = updated;
            wholeSelection = false;
            if (global || end - start > SOURCE_PAGE * 2 || (end <= start && file.document.size() != 0)) {
                long cursor = offset + added.length;
                EditorFile edited = file;
                editor.post(() -> { if (file == edited && !busy && !previewing) showPage(cursor, 0); });
            }
            refresh();
        } catch (Exception ex) { error(ex); showPage(offset, 0); }
    }

    private void record(long offset, long removed, byte[] before, byte[] after) {
        redo.clear();
        if (removed + after.length + 64 > HISTORY_BYTES) { undo.clear(); undoBytes = 0; return; }
        Change edit = new Change(offset, before, after);
        while (undoBytes + edit.cost() > HISTORY_BYTES) undoBytes -= undo.removeFirst().cost();
        undo.add(edit);
        undoBytes += edit.cost();
    }

    private void changeHistory(boolean backwards) {
        ArrayDeque<Change> from = backwards ? undo : redo;
        if (from.isEmpty()) return;
        Change edit = from.getLast();
        byte[] remove = backwards ? edit.after : edit.before;
        byte[] insert = backwards ? edit.before : edit.after;
        try {
            file.document.replace(edit.offset, remove.length, insert);
            from.removeLast();
            (backwards ? redo : undo).add(edit);
            undoBytes += backwards ? -edit.cost() : edit.cost();
            file.dirty = true;
            previewing = false;
            showPage(edit.offset + insert.length, 0);
        } catch (Exception e) { error(e); }
    }

    private class SourceEditor extends EditText {
        SourceEditor() { super(MainActivity.this); }
        @Override public boolean onTextContextMenuItem(int id) {
            if (id == android.R.id.undo || id == android.R.id.redo) {
                changeHistory(id == android.R.id.undo);
                return true;
            }
            if (id == android.R.id.selectAll && (start != 0 || end < file.document.size())) {
                selectAll();
                wholeSelection = true;
                refresh();
                return true;
            }
            if (wholeSelection && (id == android.R.id.copy || id == android.R.id.cut)) {
                long size = file.document.size();
                if (size > 256 * 1024) {
                    toast("This selection exceeds the 256 KiB clipboard limit.");
                    return true;
                }
                android.content.ClipboardManager clipboard = (android.content.ClipboardManager)getSystemService(CLIPBOARD_SERVICE);
                clipboard.setPrimaryClip(ClipData.newPlainText(file.name, new String(file.document.read(0, (int)size), UTF_8)));
                if (id == android.R.id.cut) getText().clear();
                return true;
            }
            return super.onTextContextMenuItem(id);
        }
        @Override protected void onSelectionChanged(int a, int b) {
            super.onSelectionChanged(a, b);
            // An actual caret/selection gesture cancels whole-document selection.
            // TextWatcher still sees it during replacement, before the next input.
            if (wholeSelection && !loading && getText().toString().equals(pageText) &&
                (Math.min(a,b) != 0 || Math.max(a,b) != length())) {
                wholeSelection = false;
                refresh();
            }
        }
    }

    private void menu() {
        PopupMenu menu = new PopupMenu(new ContextThemeWrapper(this,
            theme.dark ? android.R.style.Theme_Material : android.R.style.Theme_Material_Light), more);
        item(menu, "New file", true, () -> leave(this::newFile));
        item(menu, "Open file…", true, this::pickFile);
        item(menu, "Open folder…", true, this::pickFolder);
        if (workspace.hasFolder()) {
            item(menu, "Browse folder", true, workspace::browse);
            item(menu, "Find in folder…", true, workspace::find);
        }
        item(menu, "Save as…", true, () -> save(true, null));
        item(menu, "Undo", !undo.isEmpty(), () -> changeHistory(true));
        item(menu, "Redo", !redo.isEmpty(), () -> changeHistory(false));
        item(menu, "Appearance…", true, () -> theme.edit(this, this::paint));
        menu.show();
    }

    private void item(PopupMenu menu, String title, boolean enabled, Runnable action) {
        menu.getMenu().add(title).setEnabled(enabled).setOnMenuItemClickListener(i -> { action.run(); return true; });
    }

    private void newFile() {
        remember();
        file.close();
        file = new EditorFile();
        previewing = false;
        undo.clear(); redo.clear(); undoBytes = 0;
        updateTabs();
        showPage(0, 0);
    }
    private void pickFile() {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), OPEN);
    }
    void pickFolder() { startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), FOLDER); }

    private void persist(Uri uri, Intent data) {
        int flags = data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try { getContentResolver().takePersistableUriPermission(uri, flags); }
        catch (SecurityException ignored) { /* Some Open With providers grant only session access. */ }
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) {
            if (request == CREATE) afterCreate = null;
            return;
        }
        Uri uri = data.getData();
        persist(uri, data);
        if (request == OPEN) requestOpen(uri, -1, 0, null);
        else if (request == FOLDER) workspace.choose(uri);
        else if (request == CREATE) {
            Runnable follow = afterCreate; afterCreate = null;
            write(uri, follow);
        }
    }

    private void leave(Runnable action) {
        if (!file.dirty) { action.run(); return; }
        dialog().setTitle("Save changes to " + file.name + "?")
            .setPositiveButton("Save", (d, w) -> save(false, action))
            .setNegativeButton("Discard", (d, w) -> action.run())
            .setNeutralButton("Cancel", null).show();
    }

    void requestOpen(Uri target, long offset, int select, Navigation.Place place) {
        if (busy) return;
        if (target.equals(file.uri)) {
            if (offset >= 0) {
                if (select > 0) previewing = false;
                showPage(offset, select);
            }
            return;
        }
        leave(() -> {
            remember();
            runIO(() -> EditorFile.open(this, target), opened -> {
                install(opened, offset, select, place);
            });
        });
    }

    private void install(EditorFile opened, long offset, int select, Navigation.Place place) {
        file.close();
        file = opened;
        undo.clear(); redo.clear(); undoBytes = 0;
        previewing = markdown() && (place != null ? place.preview() : offset < 0);
        long point = Math.max(0, offset);
        if (place != null && place.visit() != 0) navigation.activate(place.visit());
        else navigation.visit(file.uri, file.name, point, previewing);
        updateTabs();
        showPage(point, select);
    }

    private void save(boolean saveAs, Runnable after) {
        if (saveAs || file.uri == null) {
            afterCreate = after;
            startActivityForResult(new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("text/plain")
                .addCategory(Intent.CATEGORY_OPENABLE).putExtra(Intent.EXTRA_TITLE, file.uri == null ? "Untitled.txt" : file.name), CREATE);
        } else write(file.uri, after);
    }

    private void write(Uri destination, Runnable after) {
        remember();
        long position = caret();
        runIO(() -> { file.save(this, destination); return true; }, ignored -> {
            navigation.visit(destination, file.name, position, previewing);
            updateTabs();
            refresh();
            toast("Saved " + file.name);
            if (after != null) after.run();
        });
    }

    private <T> void runIO(Callable<T> work, Consumer<T> done) {
        busy = true;
        refresh();
        back.setEnabled(false); forward.setEnabled(false);
        io.execute(() -> {
            try {
                T value = work.call();
                runOnUiThread(() -> {
                    if (isDestroyed()) {
                        if (value instanceof EditorFile abandoned) abandoned.close();
                        if (value instanceof HistoryTarget abandoned) abandoned.file().close();
                        return;
                    }
                    busy = false;
                    refresh();
                    updateTabs();
                    done.accept(value);
                });
            } catch (Exception e) {
                runOnUiThread(() -> {
                    if (isDestroyed()) return;
                    busy = false;
                    updateTabs(); refresh(); error(e);
                });
            }
        });
    }

    private void remember() {
        if (!busy && file.uri != null) navigation.remember(caret(), previewing);
    }

    private void toast(String message) { Toast.makeText(this, message, Toast.LENGTH_SHORT).show(); }
    void error(Exception error) {
        if (!isDestroyed()) dialog().setTitle("Slate").setMessage(error.getLocalizedMessage())
            .setPositiveButton("Close", null).show();
    }

    @Override public boolean onKeyShortcut(int code, KeyEvent event) {
        if (busy || !event.isCtrlPressed()) return super.onKeyShortcut(code, event);
        switch (code) {
            case KeyEvent.KEYCODE_O -> { if (event.isShiftPressed()) pickFolder(); else pickFile(); }
            case KeyEvent.KEYCODE_S -> save(event.isShiftPressed(), null);
            case KeyEvent.KEYCODE_Z -> changeHistory(!event.isShiftPressed());
            case KeyEvent.KEYCODE_N -> leave(this::newFile);
            case KeyEvent.KEYCODE_A -> { if (previewing) return super.onKeyShortcut(code, event); editor.onTextContextMenuItem(android.R.id.selectAll); }
            default -> { return super.onKeyShortcut(code, event); }
        }
        return true;
    }

    @Override public void onBackPressed() {
        if (busy) return;
        if (navigation.relative(currentTab == 0 ? 0 : -1) != null) travel(-1);
        else leave(this::finish);
    }
    @Override protected void onPause() { super.onPause(); remember(); }
    @Override protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state);
        state.putBoolean("untitled", file.uri == null);
    }
    @Override protected void onDestroy() {
        workspace.close();
        removePreview();
        EditorFile release = file;
        io.execute(() -> { release.close(); navigation.close(); });
        io.shutdown();
        super.onDestroy();
    }
}
