#include "workspace.hpp"
#include "search.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slate {
namespace {

namespace fs = std::filesystem;
constexpr unsigned page_size = 128;
constexpr std::size_t result_limit = 200;
enum Kind { folder_row, file_row, more_row, note_row };
enum Column { kind_col, icon_col, markup_col, path_col, offset_col, length_col, tooltip_col, cols };

void add_class(GtkWidget* widget, const char* name) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), name);
}

std::string display_path(const fs::path& path) {
    gchar* converted = g_filename_display_name(path.c_str());
    std::string result(converted);
    g_free(converted);
    return result;
}

std::string escape(std::string_view bytes) {
    if (bytes.empty()) return {};
    gchar* valid = g_utf8_make_valid(bytes.data(), static_cast<gssize>(bytes.size()));
    gchar* escaped = g_markup_escape_text(valid, -1);
    std::string result(escaped);
    g_free(valid);
    g_free(escaped);
    return result;
}

void later(std::function<void()> action) {
    using Work = std::function<void()>;
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, [](gpointer data) -> gboolean {
        (*static_cast<Work*>(data))();
        return G_SOURCE_REMOVE;
    }, new Work(std::move(action)), [](gpointer data) { delete static_cast<Work*>(data); });
}

void in_background(std::function<void()> action) {
    using Work = std::function<void()>;
    GTask* task = g_task_new(nullptr, nullptr, nullptr, nullptr);
    g_task_set_task_data(task, new Work(std::move(action)),
                         [](gpointer data) { delete static_cast<Work*>(data); });
    g_task_run_in_thread(task, [](GTask* task, gpointer, gpointer data, GCancellable*) {
        (*static_cast<Work*>(data))();
        g_task_return_boolean(task, TRUE);
    });
    g_object_unref(task);
}

GtkTreeStore* new_store() {
    return gtk_tree_store_new(cols, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                              G_TYPE_UINT64, G_TYPE_UINT64, G_TYPE_STRING);
}

GtkTreeIter append(GtkTreeStore* store, GtkTreeIter* parent, Kind kind, const char* icon,
                   const std::string& markup, const fs::path& path, const std::string& tip,
                   std::uint64_t offset = 0, std::size_t length = 0) {
    GtkTreeIter iter;
    gtk_tree_store_insert_with_values(store, &iter, parent, -1, kind_col, kind, icon_col, icon,
                                      markup_col, markup.c_str(), path_col, path.c_str(),
                                      offset_col, static_cast<guint64>(offset),
                                      length_col, static_cast<guint64>(length),
                                      tooltip_col, escape(tip).c_str(), -1);
    return iter;
}

std::string iter_path(GtkTreeModel* model, GtkTreeIter* iter) {
    gchar* value = nullptr;
    gtk_tree_model_get(model, iter, path_col, &value, -1);
    std::string path = value ? value : "";
    g_free(value);
    return path;
}

int row_kind(GtkTreeModel* model, GtkTreeIter* iter) {
    gint kind = note_row;
    gtk_tree_model_get(model, iter, kind_col, &kind, -1);
    return kind;
}

void remove_actions(GtkTreeStore* store, GtkTreeIter* parent) {
    auto* model = GTK_TREE_MODEL(store);
    GtkTreeIter child;
    gboolean valid = gtk_tree_model_iter_children(model, &child, parent);
    while (valid) {
        if (row_kind(model, &child) >= more_row) valid = gtk_tree_store_remove(store, &child);
        else valid = gtk_tree_model_iter_next(model, &child);
    }
}

struct Entry {
    fs::path path;
    bool directory;
};

struct Page {
    std::vector<Entry> entries;
    bool more = false;
    std::string error;
};

// Only one worker uses a given iterator at a time. Keeping it at the next
// unread entry makes each additional page independent of the directory size.
struct Cursor {
    fs::path path;
    fs::directory_iterator iter;
    bool started = false;
    SearchCancellation cancel;

    Page read() {
        Page page;
        std::error_code ec;
        if (!started) {
            iter = fs::directory_iterator(path, ec);
            started = true;
        }
        while (!ec && iter != fs::directory_iterator{} && page.entries.size() < page_size &&
               !cancel.stop_requested()) {
            auto entry = *iter;
            auto type = entry.symlink_status(ec).type();
            if (!ec && (type == fs::file_type::directory || type == fs::file_type::regular)) {
                page.entries.push_back({entry.path(), type == fs::file_type::directory});
            }
            iter.increment(ec);
        }
        if (ec) page.error = ec.message();
        page.more = !ec && iter != fs::directory_iterator{};
        return page;
    }
};

struct Folder {
    GtkTreeIter row;
    std::shared_ptr<Cursor> cursor;
    bool loading = false;
};

} // namespace

struct Workspace::Impl : std::enable_shared_from_this<Impl> {
    explicit Impl(Workspace& owner) : owner(owner) {}

    Workspace& owner;
    GtkWidget *box{}, *entry{}, *files_btn{}, *content_btn{}, *tree{}, *status{};
    GtkTreeStore *browser{}, *results{};
    fs::path root;
    std::unordered_map<fs::path, Folder> folders;
    SearchCancellation search_stop;
    guint timer = 0;
    std::uint64_t generation = 0;
    bool closed = false;
    bool search_running = false;
    bool search_pending = false;
    bool file_mode = true;

    ~Impl() {
        shutdown();
        if (box) {
            gtk_widget_destroy(box);
            g_object_unref(box);
        }
        g_clear_object(&browser);
        g_clear_object(&results);
    }

    void cancel_search() {
        ++generation;
        search_pending = false;
        search_stop.request_stop();
        if (timer != 0) { g_source_remove(timer); timer = 0; }
    }

    void clear_folders() {
        for (auto& [path, folder] : folders) folder.cursor->cancel.request_stop();
        folders.clear();
    }

    void shutdown() {
        if (closed) return;
        closed = true;
        cancel_search();
        clear_folders();
    }

    void build() {
        box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        g_object_ref_sink(box);
        gtk_widget_set_name(box, "sidebar");
        g_signal_connect(box, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer p) {
            static_cast<Impl*>(p)->shutdown();
        }), this);

        auto* controls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_widget_set_margin_start(controls, 12);
        gtk_widget_set_margin_end(controls, 12);
        gtk_widget_set_margin_top(controls, 11);
        gtk_widget_set_margin_bottom(controls, 8);
        gtk_box_pack_start(GTK_BOX(box), controls, FALSE, FALSE, 0);
        auto* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_box_pack_start(GTK_BOX(controls), top, FALSE, FALSE, 0);
        auto* title = gtk_label_new("EXPLORER");
        add_class(title, "workspace-heading");
        gtk_box_pack_start(GTK_BOX(top), title, FALSE, FALSE, 0);
        auto* refresh = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_MENU);
        gtk_widget_set_tooltip_text(refresh, "Refresh workspace");
        add_class(refresh, "workspace-refresh");
        gtk_box_pack_end(GTK_BOX(top), refresh, FALSE, FALSE, 0);
        g_signal_connect(refresh, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
            auto& self = *static_cast<Impl*>(p);
            self.set_root(self.root);
        }), this);

        entry = gtk_search_entry_new();
        add_class(entry, "workspace-search");
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Find a file…");
        gtk_entry_set_width_chars(GTK_ENTRY(entry), 10);
        gtk_box_pack_start(GTK_BOX(controls), entry, FALSE, FALSE, 0);
        auto* modes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
        gtk_box_pack_start(GTK_BOX(controls), modes, FALSE, FALSE, 0);
        files_btn = gtk_radio_button_new_with_label(nullptr, "Files");
        content_btn = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(files_btn),
                                                                  "Contents");
        for (auto* button : {files_btn, content_btn}) {
            gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(button), FALSE);
            add_class(button, "workspace-mode");
            gtk_box_pack_start(GTK_BOX(modes), button, FALSE, FALSE, 0);
            g_signal_connect_swapped(button, "clicked", G_CALLBACK(gtk_widget_grab_focus), entry);
        }
        gtk_widget_set_tooltip_text(files_btn, "Find by file name or relative path");
        gtk_widget_set_tooltip_text(content_btn, "Find exact, case-sensitive text in files");
        g_signal_connect(files_btn, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer p) {
            auto& self = *static_cast<Impl*>(p);
            self.file_mode = gtk_toggle_button_get_active(button);
            gtk_entry_set_placeholder_text(GTK_ENTRY(self.entry),
                                           self.file_mode ? "Find a file…" : "Find text in files…");
            self.query_changed();
        }), this);
        g_signal_connect(entry, "changed", G_CALLBACK(+[](GtkEntry*, gpointer p) {
            static_cast<Impl*>(p)->query_changed();
        }), this);
        g_signal_connect(entry, "stop-search", G_CALLBACK(+[](GtkSearchEntry* entry, gpointer) {
            gtk_entry_set_text(GTK_ENTRY(entry), "");
        }), nullptr);

        browser = new_store();
        results = new_store();
        gtk_tree_sortable_set_default_sort_func(GTK_TREE_SORTABLE(browser),
            [](GtkTreeModel* model, GtkTreeIter* a, GtkTreeIter* b, gpointer) -> gint {
                const int kind = row_kind(model, a) - row_kind(model, b);
                if (kind) return kind;
                const auto aa = iter_path(model, a), bb = iter_path(model, b);
                const int cmp = g_ascii_strcasecmp(aa.c_str(), bb.c_str());
                return cmp ? cmp : aa.compare(bb);
            }, nullptr, nullptr);
        gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(browser),
            GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID, GTK_SORT_ASCENDING);
        tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(browser));
        add_class(tree, "workspace-tree");
        auto* view = GTK_TREE_VIEW(tree);
        gtk_tree_view_set_headers_visible(view, FALSE);
        gtk_tree_view_set_enable_search(view, FALSE);
        gtk_tree_view_set_activate_on_single_click(view, TRUE);
        gtk_tree_view_set_tooltip_column(view, tooltip_col);
        auto* column = gtk_tree_view_column_new();
        auto* icon = gtk_cell_renderer_pixbuf_new();
        g_object_set(icon, "xpad", 3u, "stock-size", GTK_ICON_SIZE_MENU, nullptr);
        gtk_tree_view_column_pack_start(column, icon, FALSE);
        gtk_tree_view_column_add_attribute(column, icon, "icon-name", icon_col);
        auto* label = gtk_cell_renderer_text_new();
        g_object_set(label, "ellipsize", PANGO_ELLIPSIZE_END, "xpad", 4u, nullptr);
        gtk_tree_view_column_pack_start(column, label, TRUE);
        gtk_tree_view_column_add_attribute(column, label, "markup", markup_col);
        gtk_tree_view_append_column(view, column);

        auto* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(scroll), tree);
        gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
        status = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(status), 0);
        gtk_label_set_line_wrap(GTK_LABEL(status), TRUE);
        gtk_widget_set_margin_start(status, 12);
        gtk_widget_set_margin_end(status, 8);
        gtk_widget_set_margin_bottom(status, 8);
        gtk_widget_set_no_show_all(status, TRUE);
        add_class(status, "workspace-note");
        gtk_box_pack_end(GTK_BOX(box), status, FALSE, FALSE, 0);

        g_signal_connect(tree, "row-expanded", G_CALLBACK(+[](GtkTreeView*, GtkTreeIter* iter,
                                                               GtkTreePath*, gpointer p) {
            static_cast<Impl*>(p)->expand(iter);
        }), this);
        g_signal_connect(tree, "row-collapsed", G_CALLBACK(+[](GtkTreeView*, GtkTreeIter* iter,
                                                                GtkTreePath*, gpointer p) {
            static_cast<Impl*>(p)->collapse(iter);
        }), this);
        g_signal_connect(tree, "row-activated", G_CALLBACK(+[](GtkTreeView*, GtkTreePath* path,
                                                               GtkTreeViewColumn*, gpointer p) {
            static_cast<Impl*>(p)->activate(path);
        }), this);
        g_signal_connect(entry, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* key,
                                                                   gpointer p) -> gboolean {
            auto& self = *static_cast<Impl*>(p);
            if (key->keyval != GDK_KEY_Down || !self.showing_results() ||
                !gtk_tree_model_iter_n_children(GTK_TREE_MODEL(self.results), nullptr)) return FALSE;
            gtk_widget_grab_focus(self.tree);
            auto* first = gtk_tree_path_new_first();
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(self.tree), first, nullptr, FALSE);
            gtk_tree_path_free(first);
            return TRUE;
        }), this);
        g_signal_connect(entry, "activate", G_CALLBACK(+[](GtkEntry*, gpointer p) {
            auto& self = *static_cast<Impl*>(p);
            if (!self.showing_results()) return;
            auto* path = gtk_tree_path_new_first();
            self.activate(path);
            gtk_tree_path_free(path);
        }), this);
    }

    bool showing_results() const {
        return gtk_tree_view_get_model(GTK_TREE_VIEW(tree)) == GTK_TREE_MODEL(results);
    }

    GtkTreeIter append_folder(GtkTreeIter* parent, const fs::path& path, bool is_root) {
        auto label = display_path(path.filename().empty() ? path : path.filename());
        auto markup = escape(label);
        if (is_root) markup = "<b>" + markup + "</b>";
        auto row = append(browser, parent, folder_row, "folder-symbolic", markup, path,
                          display_path(path));
        append(browser, &row, note_row, nullptr, "Loading…", {}, {});
        return row;
    }

    void set_root(fs::path path) {
        if (closed) return;
        clear_folders();
        cancel_search();
        if (showing_results()) gtk_tree_view_set_model(GTK_TREE_VIEW(tree), GTK_TREE_MODEL(browser));
        gtk_tree_store_clear(browser);
        std::error_code ec;
        root = fs::absolute(path, ec).lexically_normal();
        if (ec) root = std::move(path);
        if (!root.has_filename() && root != root.root_path()) root = root.parent_path();
        append_folder(nullptr, root, true);
        auto* row = gtk_tree_path_new_first();
        gtk_tree_view_expand_row(GTK_TREE_VIEW(tree), row, FALSE);
        gtk_tree_path_free(row);
        query_changed();
    }

    void expand(GtkTreeIter* row) {
        if (closed || showing_results()) return;
        fs::path path(iter_path(GTK_TREE_MODEL(browser), row));
        if (folders.contains(path)) return;
        auto cursor = std::make_shared<Cursor>();
        cursor->path = path;
        folders.emplace(path, Folder{*row, cursor});
        load_page(path);
    }

    void collapse(GtkTreeIter* row) {
        if (closed || showing_results()) return;
        auto* model = GTK_TREE_MODEL(browser);
        fs::path path(iter_path(model, row));
        for (auto it = folders.begin(); it != folders.end();) {
            const auto relative = it->first.lexically_relative(path);
            if (!relative.empty() && *relative.begin() != "..") {
                it->second.cursor->cancel.request_stop();
                it = folders.erase(it);
            } else ++it;
        }
        // Leave a child so GTK continues to show the disclosure triangle.
        auto placeholder = append(browser, row, note_row, nullptr, "Loading…", {}, {});
        GtkTreeIter child;
        gboolean valid = gtk_tree_model_iter_children(model, &child, row);
        while (valid) {
            if (child.user_data == placeholder.user_data) valid = gtk_tree_model_iter_next(model, &child);
            else valid = gtk_tree_store_remove(browser, &child);
        }
    }

    void load_page(const fs::path& path) {
        auto found = folders.find(path);
        if (found == folders.end() || found->second.loading) return;
        found->second.loading = true;
        auto cursor = found->second.cursor;
        auto weak = weak_from_this();
        in_background([weak, cursor]() {
            auto page = cursor->read();
            later([weak, cursor, page = std::move(page)] {
                auto self = weak.lock();
                if (!self || self->closed || cursor->cancel.stop_requested()) return;
                self->loaded(cursor, page);
            });
        });
    }

    void loaded(const std::shared_ptr<Cursor>& cursor, const Page& page) {
        auto found = folders.find(cursor->path);
        if (found == folders.end() || found->second.cursor != cursor) return;
        found->second.loading = false;
        auto row = found->second.row;
        for (const auto& item : page.entries) {
            if (item.directory) append_folder(&row, item.path, false);
            else append(browser, &row, file_row, "text-x-generic-symbolic",
                        escape(display_path(item.path.filename())), item.path, display_path(item.path));
        }
        // Install the replacement before removing the placeholder to preserve
        // expansion when a directory is empty or inaccessible.
        std::string message;
        if (!page.error.empty()) message = "Cannot read folder";
        else if (page.more) message = "Load more…";
        else if (gtk_tree_model_iter_n_children(GTK_TREE_MODEL(browser), &row) == 1 &&
                 page.entries.empty()) message = "Empty folder";
        if (message.empty()) { remove_actions(browser, &row); return; }
        auto* model = GTK_TREE_MODEL(browser);
        GtkTreeIter child;
        if (gtk_tree_model_iter_children(model, &child, &row)) {
            do {
                if (row_kind(model, &child) < more_row) continue;
                gtk_tree_store_set(browser, &child, kind_col, page.more ? more_row : note_row,
                                   markup_col, message.c_str(), path_col, cursor->path.c_str(),
                                   tooltip_col, escape(page.error.empty() ? message : page.error).c_str(), -1);
                break;
            } while (gtk_tree_model_iter_next(model, &child));
        }
    }

    void activate(GtkTreePath* path) {
        auto* view = GTK_TREE_VIEW(tree);
        auto* model = gtk_tree_view_get_model(view);
        GtkTreeIter iter;
        if (!gtk_tree_model_get_iter(model, &iter, path)) return;
        const int kind = row_kind(model, &iter);
        if (kind == folder_row) {
            if (gtk_tree_view_row_expanded(view, path)) gtk_tree_view_collapse_row(view, path);
            else gtk_tree_view_expand_row(view, path, FALSE);
        } else if (kind == more_row) {
            load_page(iter_path(model, &iter));
        } else if (kind == file_row && owner.on_open) {
            guint64 offset = 0, length = 0;
            gtk_tree_model_get(model, &iter, offset_col, &offset, length_col, &length, -1);
            owner.on_open(iter_path(model, &iter), offset, static_cast<std::size_t>(length));
        }
    }

    void query_changed() {
        if (closed || !browser) return;
        cancel_search();
        gtk_tree_store_clear(results);
        const std::string query = gtk_entry_get_text(GTK_ENTRY(entry));
        auto* view = GTK_TREE_VIEW(tree);
        if (query.empty()) {
            if (showing_results()) {
                gtk_tree_view_set_model(view, GTK_TREE_MODEL(browser));
                gtk_tree_view_set_show_expanders(view, TRUE);
                // GTK drops expansion when swapping models. Restore every loaded
                // folder, including its ancestors, so its retained pages stay visible.
                for (auto& [path, folder] : folders) {
                    auto* row = gtk_tree_model_get_path(GTK_TREE_MODEL(browser), &folder.row);
                    gtk_tree_view_expand_to_path(view, row);
                    gtk_tree_path_free(row);
                }
            }
            gtk_widget_hide(status);
            return;
        }
        if (!showing_results()) {
            gtk_tree_view_set_model(view, GTK_TREE_MODEL(results));
            gtk_tree_view_set_show_expanders(view, FALSE);
        }
        gtk_label_set_text(GTK_LABEL(status), "Searching…");
        gtk_widget_show(status);
        // Name queries only enumerate directory entries. Let content queries
        // settle longer before starting disk reads on each keystroke.
        timer = g_timeout_add(file_mode ? 24 : 110, [](gpointer data) -> gboolean {
            auto& self = *static_cast<Impl*>(data);
            self.timer = 0;
            self.start_search();
            return G_SOURCE_REMOVE;
        }, this);
    }

    void start_search() {
        if (search_running) { search_pending = true; return; }
        search_running = true;
        search_pending = false;
        const std::string query = gtk_entry_get_text(GTK_ENTRY(entry));
        const auto mode = file_mode ? SearchMode::filenames : SearchMode::contents;
        search_stop = SearchCancellation();
        auto weak = weak_from_this();
        auto stop = search_stop;
        auto serial = generation;
        in_background([weak, stop, serial, path = root, query, mode] {
            std::vector<SearchHit> batch;
            auto last = std::chrono::steady_clock::now() - std::chrono::seconds(1);
            auto flush = [&] {
                if (batch.empty()) return;
                later([weak, serial, hits = std::move(batch)] {
                    auto self = weak.lock();
                    if (self && !self->closed && self->generation == serial) self->add_hits(hits);
                });
                batch.clear();
                last = std::chrono::steady_clock::now();
            };
            auto summary = search_workspace(path, query, mode, result_limit, stop, [&](SearchHit hit) {
                batch.push_back(std::move(hit));
                if (batch.size() >= 24 || std::chrono::steady_clock::now() - last >
                                          std::chrono::milliseconds(50)) flush();
            });
            if (!stop.stop_requested()) flush();
            later([weak, serial, summary] {
                auto self = weak.lock();
                if (!self || self->closed) return;
                self->search_running = false;
                if (self->search_pending) { self->start_search(); return; }
                if (self->generation != serial) return;
                std::string text;
                if (summary.reached_limit) text = "First " + std::to_string(result_limit) + " matches";
                else if (summary.matches == 0) text = "No matches";
                else text = std::to_string(summary.matches) + (summary.matches == 1 ? " match" : " matches");
                if (summary.errors) text += " · " + std::to_string(summary.errors) + " unreadable";
                gtk_label_set_text(GTK_LABEL(self->status), text.c_str());
            });
        });
    }

    void add_hits(const std::vector<SearchHit>& hits) {
        for (const auto& hit : hits) {
            const auto rel = display_path(hit.path.lexically_relative(root));
            const auto name = escape(display_path(hit.path.filename()));
            const auto detail = hit.length ? escape(hit.excerpt) : escape(rel);
            auto markup = name;
            if (!detail.empty()) markup += "\n<span size='small'>" + detail + "</span>";
            auto tip = rel;
            if (hit.length) tip += " · byte " + std::to_string(hit.offset);
            append(results, nullptr, file_row, "text-x-generic-symbolic", markup, hit.path, tip,
                   hit.offset, hit.length);
        }
    }
};

Workspace::Workspace(const fs::path& root) : impl_(std::make_shared<Impl>(*this)) {
    impl_->build();
    impl_->set_root(root);
}

Workspace::~Workspace() = default;

GtkWidget* Workspace::widget() const { return impl_->box; }

void Workspace::set_root(const fs::path& path) { impl_->set_root(path); }

void Workspace::focus_search(bool content) {
    if (impl_->closed) return;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(content ? impl_->content_btn : impl_->files_btn), TRUE);
    gtk_widget_grab_focus(impl_->entry);
    gtk_editable_select_region(GTK_EDITABLE(impl_->entry), 0, -1);
}

} // namespace slate
